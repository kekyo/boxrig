/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_UI_TEXT_LAYOUT_H
#define BOXRIG_UI_TEXT_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque identifier for one renderer-owned font resource. */
typedef uint64_t boxrig_font_resource_id;

/** A zero identifier selects no registered font resource. */
#define BOXRIG_FONT_RESOURCE_NONE UINT64_C(0)

/** Base direction policy used when resolving a terminal row. */
typedef enum boxrig_text_direction_policy {
    BOXRIG_TEXT_DIRECTION_AUTO = 0,
    BOXRIG_TEXT_DIRECTION_FORCE_LTR,
    BOXRIG_TEXT_DIRECTION_FORCE_RTL
} boxrig_text_direction_policy;

/** Resolved direction of a text run. */
typedef enum boxrig_text_direction {
    BOXRIG_TEXT_DIRECTION_LTR = 0,
    BOXRIG_TEXT_DIRECTION_RTL
} boxrig_text_direction;

/** Portable result codes returned by text layout operations. */
typedef enum boxrig_text_layout_status {
    BOXRIG_TEXT_LAYOUT_OK = 0,
    BOXRIG_TEXT_LAYOUT_INVALID_ARGUMENT,
    BOXRIG_TEXT_LAYOUT_OUT_OF_MEMORY
} boxrig_text_layout_status;

/** One logical grapheme cluster supplied by a platform text analyzer. */
typedef struct boxrig_text_cluster_input {
    /** First logical terminal column occupied by the cluster. */
    size_t logical_column;
    /** Number of indivisible terminal columns occupied by the cluster. */
    size_t column_count;
    /** Byte offset in the analyzer's immutable logical UTF-8 row. */
    size_t text_offset;
    /** Byte length in the analyzer's immutable logical UTF-8 row. */
    size_t text_length;
    /** Unicode bidirectional embedding level resolved for the cluster. */
    uint8_t bidi_level;
    /** Renderer-defined key for shaping-relevant cell style. */
    uint64_t style_key;
    /** Renderer-defined key for hyperlink metadata. */
    uint64_t hyperlink_key;
    /** Renderer-defined key for the resolved fallback font. */
    uint64_t font_key;
    /** Renderer-defined key for the resolved Unicode script. */
    uint64_t script_key;
    /** Renderer-defined key for shaping language. */
    uint64_t language_key;
    /** Preserve natural advance and center it within the allocated columns. */
    bool preserve_advance;
} boxrig_text_cluster_input;

/** One shaping context in logical and visual terminal coordinates. */
typedef struct boxrig_text_layout_run {
    /** First logical column in the run. */
    size_t logical_column;
    /** Number of logical columns in the run. */
    size_t logical_column_count;
    /** Leftmost visual column occupied by the run. */
    size_t visual_column;
    /** Number of visual columns occupied by the run. */
    size_t visual_column_count;
    /** Resolved run direction. */
    boxrig_text_direction direction;
    /** Unicode bidirectional embedding level shared by the run. */
    uint8_t bidi_level;
    /** Shaping-relevant style key shared by the run. */
    uint64_t style_key;
    /** Hyperlink key shared by the run. */
    uint64_t hyperlink_key;
    /** Resolved font key shared by the run. */
    uint64_t font_key;
    /** Unicode script key shared by the run. */
    uint64_t script_key;
    /** Language key shared by the run. */
    uint64_t language_key;
} boxrig_text_layout_run;

/** A half-open interval in terminal columns. */
typedef struct boxrig_text_layout_range {
    /** First column in the interval. */
    size_t column;
    /** Number of columns in the interval. */
    size_t column_count;
} boxrig_text_layout_range;

/** Opaque immutable mapping for one terminal row. */
typedef struct boxrig_text_layout boxrig_text_layout;

/** Portable font slant. */
typedef enum boxrig_font_style {
    BOXRIG_FONT_STYLE_NORMAL = 0,
    BOXRIG_FONT_STYLE_ITALIC,
    BOXRIG_FONT_STYLE_OBLIQUE
} boxrig_font_style;

/** Portable font stretch. */
typedef enum boxrig_font_stretch {
    BOXRIG_FONT_STRETCH_CONDENSED = 0,
    BOXRIG_FONT_STRETCH_NORMAL,
    BOXRIG_FONT_STRETCH_EXPANDED
} boxrig_font_stretch;

/** Font fallback scope. */
typedef enum boxrig_font_fallback_policy {
    BOXRIG_FONT_FALLBACK_SYSTEM = 0,
    BOXRIG_FONT_FALLBACK_REGISTERED_ONLY,
    BOXRIG_FONT_FALLBACK_FAMILIES_ONLY
} boxrig_font_fallback_policy;

/** Primary-face monospace requirement. */
typedef enum boxrig_font_monospace_policy {
    BOXRIG_FONT_MONOSPACE_REQUIRED = 0,
    BOXRIG_FONT_MONOSPACE_PREFERRED,
    BOXRIG_FONT_MONOSPACE_UNRESTRICTED
} boxrig_font_monospace_policy;

/** One preferred UTF-8 family and optional exact registered resource. */
typedef struct boxrig_font_family {
    /** Length-delimited UTF-8 family name, or NULL when selecting by resource. */
    const char *name;
    /** Number of bytes in name. */
    size_t name_length;
    /** Exact resource preference, or BOXRIG_FONT_RESOURCE_NONE. */
    boxrig_font_resource_id resource_id;
} boxrig_font_family;

/** One OpenType feature value. */
typedef struct boxrig_font_feature {
    /** Four-byte OpenType tag in big-endian byte order. */
    uint32_t tag;
    /** Feature value, normally zero or one. */
    uint32_t value;
} boxrig_font_feature;

/** One OpenType variable-font axis value. */
typedef struct boxrig_font_variation {
    /** Four-byte OpenType axis tag in big-endian byte order. */
    uint32_t tag;
    /** Requested design-space value. */
    float value;
} boxrig_font_variation;

/** Platform-independent candidate font specification. */
typedef struct boxrig_font_specification {
    /** Preferred families in selection order. */
    const boxrig_font_family *families;
    /** Number of preferred families. */
    size_t family_count;
    /** Requested font size in 1/96-inch device-independent pixels. */
    float size_dip;
    /** Additional line height in device-independent pixels. */
    float line_height_adjustment_dip;
    /** OpenType weight from 1 through 1000. */
    unsigned int weight;
    /** Requested slant. */
    boxrig_font_style style;
    /** Requested stretch. */
    boxrig_font_stretch stretch;
    /** Permitted fallback scope. */
    boxrig_font_fallback_policy fallback;
    /** Monospace selection requirement for the primary face. */
    boxrig_font_monospace_policy monospace;
    /** Optional OpenType features. */
    const boxrig_font_feature *features;
    /** Number of OpenType features. */
    size_t feature_count;
    /** Optional variable-font axes. */
    const boxrig_font_variation *variations;
    /** Number of variable-font axes. */
    size_t variation_count;
} boxrig_font_specification;

/** Effective primary-face metrics resolved for one output. */
typedef struct boxrig_font_effective_metrics {
    /** Raw requested font size in device-independent pixels. */
    float requested_size_dip;
    /** Exact registered resource, or BOXRIG_FONT_RESOURCE_NONE. */
    boxrig_font_resource_id primary_resource_id;
    /** Renderer-defined nonzero identity for the resolved primary face. */
    uint64_t primary_face_key;
    /** Horizontal output resolution used for resolution. */
    float dpi_x;
    /** Vertical output resolution used for resolution. */
    float dpi_y;
    /** Additional output scale used for resolution. */
    float output_scale;
    /** Measured primary glyph width in output pixels. */
    float glyph_width;
    /** Measured primary glyph height in output pixels. */
    float glyph_height;
    /** Primary-face ascent in output pixels. */
    float ascent;
    /** Primary-face descent in output pixels. */
    float descent;
    /** Primary-face natural line gap in output pixels, which may be negative. */
    float line_gap;
    /** Effective explicit line-height addition in output pixels. */
    float line_height_adjustment;
    /** Final integral terminal cell width in output pixels. */
    unsigned int cell_width;
    /** Final integral terminal cell height in output pixels. */
    unsigned int cell_height;
    /** Final integral baseline offset from the cell top. */
    unsigned int baseline;
    /** Renderer layout revision committed with these metrics. */
    uint64_t layout_revision;
} boxrig_font_effective_metrics;

/** Direction of one user-visible font zoom operation. */
typedef enum boxrig_font_zoom_direction {
    BOXRIG_FONT_ZOOM_DECREASE = 0,
    BOXRIG_FONT_ZOOM_INCREASE
} boxrig_font_zoom_direction;

/** Candidate range used to build deterministic visual font steps. */
typedef struct boxrig_font_visual_step_policy {
    /** Smallest candidate size in device-independent pixels. */
    float minimum_size_dip;
    /** Largest candidate size in device-independent pixels. */
    float maximum_size_dip;
    /** Distance between resolved candidate sizes. */
    float candidate_increment_dip;
} boxrig_font_visual_step_policy;

/** Opaque deterministic sequence of effective visual font steps. */
typedef struct boxrig_font_visual_step_sequence
    boxrig_font_visual_step_sequence;

/** Portable antialiasing policy for one output. */
typedef enum boxrig_font_antialias {
    BOXRIG_FONT_ANTIALIAS_SYSTEM = 0,
    BOXRIG_FONT_ANTIALIAS_GRAYSCALE,
    BOXRIG_FONT_ANTIALIAS_SUBPIXEL,
    BOXRIG_FONT_ANTIALIAS_NONE
} boxrig_font_antialias;

/** Portable font hinting policy for one output. */
typedef enum boxrig_font_hinting {
    BOXRIG_FONT_HINTING_SYSTEM = 0,
    BOXRIG_FONT_HINTING_ENABLED,
    BOXRIG_FONT_HINTING_DISABLED
} boxrig_font_hinting;

/** Physical subpixel order of one output. */
typedef enum boxrig_font_subpixel_order {
    BOXRIG_FONT_SUBPIXEL_UNKNOWN = 0,
    BOXRIG_FONT_SUBPIXEL_RGB,
    BOXRIG_FONT_SUBPIXEL_BGR,
    BOXRIG_FONT_SUBPIXEL_VRGB,
    BOXRIG_FONT_SUBPIXEL_VBGR
} boxrig_font_subpixel_order;

/** Font rasterization settings owned by a view or output. */
typedef struct boxrig_font_rendering_policy {
    /** Horizontal output resolution. */
    float dpi_x;
    /** Vertical output resolution. */
    float dpi_y;
    /** Additional compositor or UI output scale. */
    float output_scale;
    /** Requested antialiasing mode. */
    boxrig_font_antialias antialias;
    /** Requested hinting mode. */
    boxrig_font_hinting hinting;
    /** Whether fractional glyph positions are permitted. */
    bool fractional_positions;
    /** Physical subpixel order. */
    boxrig_font_subpixel_order subpixel_order;
    /** Whether color glyph rendering is permitted. */
    bool color_glyphs;
} boxrig_font_rendering_policy;

/** Resource limits enforced by a renderer-owned font catalog. */
typedef struct boxrig_font_catalog_limits {
    /** Maximum registered font files or immutable byte resources. */
    size_t maximum_files;
    /** Maximum aggregate bytes retained or referenced by resources. */
    size_t maximum_bytes;
    /** Maximum aggregate font faces exposed by resources. */
    size_t maximum_faces;
} boxrig_font_catalog_limits;

/** Result codes returned by portable font catalog operations. */
typedef enum boxrig_font_catalog_status {
    BOXRIG_FONT_CATALOG_OK = 0,
    BOXRIG_FONT_CATALOG_INVALID_ARGUMENT,
    BOXRIG_FONT_CATALOG_OUT_OF_MEMORY,
    BOXRIG_FONT_CATALOG_IO_ERROR,
    BOXRIG_FONT_CATALOG_INVALID_FONT,
    BOXRIG_FONT_CATALOG_LIMIT_EXCEEDED,
    BOXRIG_FONT_CATALOG_NOT_FOUND
} boxrig_font_catalog_status;

/**
 * Resolves natural primary-face metrics for one candidate size.
 *
 * @param requested_size_dip Candidate size in device-independent pixels.
 * @param context Opaque resolver context.
 * @param metrics_out Receives natural metrics with no line-height addition.
 * @return A portable font catalog status.
 *
 * @remarks The callback is invoked synchronously and is never retained.
 */
typedef boxrig_font_catalog_status (*boxrig_font_metrics_resolver)(
    float requested_size_dip,
    void *context,
    boxrig_font_effective_metrics *metrics_out);

/** Source kind retained for one registered font resource. */
typedef enum boxrig_font_resource_kind {
    BOXRIG_FONT_RESOURCE_FILE = 0,
    BOXRIG_FONT_RESOURCE_MEMORY
} boxrig_font_resource_kind;

/** Candidate passed to a platform font-engine validator. */
typedef struct boxrig_font_resource_candidate {
    /** Candidate source kind. */
    boxrig_font_resource_kind kind;
    /** Length-delimited UTF-8 path for file resources, otherwise NULL. */
    const char *path;
    /** Number of bytes in path. */
    size_t path_length;
    /** Immutable bytes for memory resources, otherwise NULL. */
    const uint8_t *data;
    /** Number of immutable bytes in data. */
    size_t data_length;
    /** Requested zero-based face in the font resource. */
    size_t face_index;
} boxrig_font_resource_candidate;

/** Immutable catalog-owned view of one registered font resource. */
typedef struct boxrig_font_resource {
    /** Opaque identifier used by a font specification. */
    boxrig_font_resource_id id;
    /** Registered source kind. */
    boxrig_font_resource_kind kind;
    /** Catalog-owned UTF-8 path for file resources, otherwise NULL. */
    const char *path;
    /** Number of bytes in path. */
    size_t path_length;
    /** Catalog-owned immutable bytes for memory resources, otherwise NULL. */
    const uint8_t *data;
    /** Number of immutable bytes available through data. */
    size_t data_length;
    /** Bytes charged against the catalog limit. */
    size_t byte_count;
    /** Selected zero-based face index. */
    size_t face_index;
    /** Number of faces validated in the complete resource. */
    size_t face_count;
} boxrig_font_resource;

/** Opaque bounded catalog of application font resources. */
typedef struct boxrig_font_catalog boxrig_font_catalog;

/**
 * Validates one candidate with a platform font engine.
 *
 * @param candidate Candidate whose borrowed fields remain valid for the call.
 * @param byte_count_out Receives the resource size charged to limits.
 * @param face_count_out Receives the number of validated font faces.
 * @param context Opaque context supplied when the catalog was created.
 * @return A font catalog status. Invalid fonts return
 * BOXRIG_FONT_CATALOG_INVALID_FONT.
 *
 * @remarks The callback validates only and must not retain candidate pointers
 * or register the resource with a platform font collection.
 */
typedef boxrig_font_catalog_status (*boxrig_font_resource_validator)(
    const boxrig_font_resource_candidate *candidate,
    size_t *byte_count_out,
    size_t *face_count_out,
    void *context);

/**
 * Builds visual mappings from analyzer-resolved grapheme clusters.
 *
 * @param clusters Logical-order clusters covering every column exactly once.
 * @param cluster_count Number of clusters.
 * @param column_count Number of logical and visual terminal columns.
 * @param base_direction Resolved paragraph base direction.
 * @param layout_out Receives an allocated immutable layout.
 * @return A portable layout status.
 *
 * @remarks A cluster is indivisible even when it occupies two columns. Runs
 * split at level, style, hyperlink, fallback-font, script, or language changes.
 */
boxrig_text_layout_status boxrig_text_layout_create(
    const boxrig_text_cluster_input *clusters,
    size_t cluster_count,
    size_t column_count,
    boxrig_text_direction base_direction,
    boxrig_text_layout **layout_out);

/**
 * Retains another owner reference to an immutable text layout.
 *
 * @param layout Layout to retain.
 * @return The same layout, or NULL for an invalid layout or reference overflow.
 */
boxrig_text_layout *boxrig_text_layout_retain(boxrig_text_layout *layout);

/**
 * Releases an immutable text layout.
 *
 * @param layout Layout to release. NULL is accepted.
 */
void boxrig_text_layout_destroy(boxrig_text_layout *layout);

/**
 * Estimates the allocation bytes retained by an immutable text layout.
 *
 * @param layout Layout to inspect.
 * @return Positive retained bytes, or zero for NULL or arithmetic overflow.
 */
size_t boxrig_text_layout_estimated_bytes(
    const boxrig_text_layout *layout);

/**
 * Returns the number of logical and visual columns in a layout.
 *
 * @param layout Layout to inspect.
 * @return Column count, or zero for NULL.
 */
size_t boxrig_text_layout_column_count(const boxrig_text_layout *layout);

/**
 * Returns the number of shaping runs in logical order.
 *
 * @param layout Layout to inspect.
 * @return Shaping run count, or zero for NULL.
 */
size_t boxrig_text_layout_run_count(const boxrig_text_layout *layout);

/**
 * Reads one shaping run in logical order.
 *
 * @param layout Layout to inspect.
 * @param run_index Zero-based run index.
 * @param run_out Receives a copied portable run description.
 * @return BOXRIG_TEXT_LAYOUT_OK or BOXRIG_TEXT_LAYOUT_INVALID_ARGUMENT.
 */
boxrig_text_layout_status boxrig_text_layout_get_run(
    const boxrig_text_layout *layout,
    size_t run_index,
    boxrig_text_layout_run *run_out);

/**
 * Maps one logical terminal column to its visual column.
 *
 * @param layout Layout to inspect.
 * @param logical_column Zero-based logical column.
 * @param visual_column_out Receives the zero-based visual column.
 * @return BOXRIG_TEXT_LAYOUT_OK or BOXRIG_TEXT_LAYOUT_INVALID_ARGUMENT.
 */
boxrig_text_layout_status boxrig_text_layout_logical_to_visual(
    const boxrig_text_layout *layout,
    size_t logical_column,
    size_t *visual_column_out);

/**
 * Maps one visual terminal column to its logical column for hit testing.
 *
 * @param layout Layout to inspect.
 * @param visual_column Zero-based visual column.
 * @param logical_column_out Receives the zero-based logical column.
 * @return BOXRIG_TEXT_LAYOUT_OK or BOXRIG_TEXT_LAYOUT_INVALID_ARGUMENT.
 */
boxrig_text_layout_status boxrig_text_layout_visual_to_logical(
    const boxrig_text_layout *layout,
    size_t visual_column,
    size_t *logical_column_out);

/**
 * Resolves logical dirty to visual shaping-context ranges.
 *
 * @param layout Layout to inspect.
 * @param logical_column First dirty logical column.
 * @param logical_column_count Number of dirty logical columns.
 * @param ranges_out Optional destination sorted by visual column.
 * @param range_capacity Number of elements available in ranges_out.
 * @return Number of required ranges, which may exceed range_capacity, or zero
 * for an invalid or empty logical interval.
 */
size_t boxrig_text_layout_dirty_ranges(
    const boxrig_text_layout *layout,
    size_t logical_column,
    size_t logical_column_count,
    boxrig_text_layout_range *ranges_out,
    size_t range_capacity);

/**
 * Maps an exact logical interval to all selected visual cell intervals.
 *
 * @param layout Layout to inspect.
 * @param logical_column First selected logical column.
 * @param logical_column_count Number of selected logical columns.
 * @param ranges_out Optional destination sorted by visual column.
 * @param range_capacity Number of elements available in ranges_out.
 * @return Number of required coalesced ranges, which may exceed
 *         range_capacity, or zero for an invalid or empty interval.
 *
 * @remarks Unlike boxrig_text_layout_dirty_ranges, this function does not
 * expand the result to complete shaping runs.
 */
size_t boxrig_text_layout_visual_ranges(
    const boxrig_text_layout *layout,
    size_t logical_column,
    size_t logical_column_count,
    boxrig_text_layout_range *ranges_out,
    size_t range_capacity);

/**
 * Returns a conservative default terminal font specification.
 *
 * @return Default specification using a required monospace system face.
 */
boxrig_font_specification boxrig_font_specification_default(void);

/**
 * Returns output font rendering defaults at 96 DPI and unit scale.
 *
 * @return Default output rendering policy.
 */
boxrig_font_rendering_policy boxrig_font_rendering_policy_default(void);

/**
 * Returns bounded default font catalog limits.
 *
 * @return Default file, byte, and face limits.
 */
boxrig_font_catalog_limits boxrig_font_catalog_limits_default(void);

/**
 * Creates an empty renderer-owned application font catalog.
 *
 * @param limits Aggregate resource limits.
 * @param validator Optional platform font-engine validator. Registration is
 * rejected when this is NULL.
 * @param validator_context Opaque value passed to validator.
 * @param catalog_out Receives an allocated catalog.
 * @return A font catalog status.
 */
boxrig_font_catalog_status boxrig_font_catalog_create(
    boxrig_font_catalog_limits limits,
    boxrig_font_resource_validator validator,
    void *validator_context,
    boxrig_font_catalog **catalog_out);

/**
 * Releases a font catalog and all copied paths and memory resources.
 *
 * @param catalog Catalog to release. NULL is accepted.
 */
void boxrig_font_catalog_destroy(boxrig_font_catalog *catalog);

/**
 * Registers one face selected from a font file path.
 *
 * @param catalog Destination catalog.
 * @param path Length-delimited UTF-8 path.
 * @param path_length Number of bytes in path.
 * @param face_index Zero-based face to select from the resource.
 * @param resource_id_out Receives a nonzero opaque identifier.
 * @return A font catalog status.
 */
boxrig_font_catalog_status boxrig_font_catalog_register_file(
    boxrig_font_catalog *catalog,
    const char *path,
    size_t path_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out);

/**
 * Copies and registers one face selected from immutable font bytes.
 *
 * @param catalog Destination catalog.
 * @param data Immutable font bytes copied before the call returns.
 * @param data_length Number of bytes in data.
 * @param face_index Zero-based face to select from the resource.
 * @param resource_id_out Receives a nonzero opaque identifier.
 * @return A font catalog status.
 */
boxrig_font_catalog_status boxrig_font_catalog_register_memory(
    boxrig_font_catalog *catalog,
    const uint8_t *data,
    size_t data_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out);

/**
 * Removes one resource and invalidates its identifier.
 *
 * @param catalog Catalog containing the resource.
 * @param resource_id Identifier returned by a registration operation.
 * @return BOXRIG_FONT_CATALOG_OK or BOXRIG_FONT_CATALOG_NOT_FOUND.
 */
boxrig_font_catalog_status boxrig_font_catalog_remove(
    boxrig_font_catalog *catalog,
    boxrig_font_resource_id resource_id);

/**
 * Reads an immutable resource view.
 *
 * @param catalog Catalog to inspect.
 * @param resource_id Resource identifier.
 * @param resource_out Receives a borrowed view valid until catalog mutation.
 * @return BOXRIG_FONT_CATALOG_OK or BOXRIG_FONT_CATALOG_NOT_FOUND.
 */
boxrig_font_catalog_status boxrig_font_catalog_get_resource(
    const boxrig_font_catalog *catalog,
    boxrig_font_resource_id resource_id,
    boxrig_font_resource *resource_out);

/**
 * Returns the current catalog revision.
 *
 * @param catalog Catalog to inspect.
 * @return A nonzero revision, or zero for NULL.
 */
uint64_t boxrig_font_catalog_revision(const boxrig_font_catalog *catalog);

/**
 * Returns current aggregate catalog usage.
 *
 * @param catalog Catalog to inspect.
 * @param file_count_out Receives registered resource count.
 * @param byte_count_out Receives aggregate validated bytes.
 * @param face_count_out Receives aggregate validated faces.
 * @return BOXRIG_FONT_CATALOG_OK or BOXRIG_FONT_CATALOG_INVALID_ARGUMENT.
 */
boxrig_font_catalog_status boxrig_font_catalog_usage(
    const boxrig_font_catalog *catalog,
    size_t *file_count_out,
    size_t *byte_count_out,
    size_t *face_count_out);

/**
 * Validates a font specification and all referenced resource identifiers.
 *
 * @param catalog Optional resource catalog.
 * @param specification Specification to validate.
 * @return A font catalog status.
 */
boxrig_font_catalog_status boxrig_font_specification_validate(
    const boxrig_font_catalog *catalog,
    const boxrig_font_specification *specification);

/**
 * Validates portable output and rasterization settings.
 *
 * @param policy Rendering policy to validate.
 * @return A font catalog status.
 */
boxrig_font_catalog_status boxrig_font_rendering_policy_validate(
    const boxrig_font_rendering_policy *policy);

/**
 * Returns the default bounded candidate range for visual font zoom.
 *
 * @return A policy spanning 6 through 72 DIP in 0.1 DIP increments.
 */
boxrig_font_visual_step_policy boxrig_font_visual_step_policy_default(void);

/**
 * Applies explicit extra line height without changing primary-face metrics.
 *
 * @param natural_metrics Valid natural metrics with no prior adjustment.
 * @param adjustment_dip Nonnegative additional height in logical pixels.
 * @param metrics_out Receives adjusted effective metrics.
 * @return A portable font catalog status.
 */
boxrig_font_catalog_status boxrig_font_effective_metrics_apply_line_height(
    const boxrig_font_effective_metrics *natural_metrics,
    float adjustment_dip,
    boxrig_font_effective_metrics *metrics_out);

/**
 * Creates a lazily resolved visual-step sequence around the current state.
 *
 * @param current_metrics Current committed effective metrics.
 * @param line_height_adjustment_dip Explicit line-height addition retained for
 * every candidate.
 * @param policy Candidate size range and increment.
 * @param resolver Synchronous platform metrics resolver retained by the
 * sequence.
 * @param resolver_context Opaque value passed to resolver. It must remain valid
 * until the sequence is destroyed.
 * @param sequence_out Receives an allocated step sequence.
 * @return A portable font catalog status.
 *
 * @remarks Candidate metrics are resolved only when an adjacent step is first
 * requested. Consecutive candidates with the same cell width collapse into one
 * visual step, and resolved adjacent states are cached in both directions. The
 * current state is retained as its width group's canonical element so an
 * increase followed by a decrease returns to exactly the same requested size
 * and metrics.
 */
boxrig_font_catalog_status boxrig_font_visual_step_sequence_create(
    const boxrig_font_effective_metrics *current_metrics,
    float line_height_adjustment_dip,
    boxrig_font_visual_step_policy policy,
    boxrig_font_metrics_resolver resolver,
    void *resolver_context,
    boxrig_font_visual_step_sequence **sequence_out);

/**
 * Releases a visual font step sequence.
 *
 * @param sequence Sequence to release. NULL is accepted.
 */
void boxrig_font_visual_step_sequence_destroy(
    boxrig_font_visual_step_sequence *sequence);

/**
 * Reads the current element of a visual font step sequence.
 *
 * @param sequence Sequence to inspect.
 * @param metrics_out Receives the current element.
 * @return True on success, or false for invalid input.
 */
bool boxrig_font_visual_step_sequence_current(
    const boxrig_font_visual_step_sequence *sequence,
    boxrig_font_effective_metrics *metrics_out);

/**
 * Reads the adjacent effective state without changing the sequence.
 *
 * @param sequence Sequence to inspect.
 * @param direction Requested visual zoom direction.
 * @param metrics_out Receives the adjacent state when one exists.
 * @return True when a visual step exists, or false at a bound or for invalid
 * input.
 */
bool boxrig_font_visual_step_sequence_peek(
    boxrig_font_visual_step_sequence *sequence,
    boxrig_font_zoom_direction direction,
    boxrig_font_effective_metrics *metrics_out);

/**
 * Advances to the adjacent effective state.
 *
 * @param sequence Sequence to update.
 * @param direction Requested visual zoom direction.
 * @return True when the sequence changed, or false at a bound or for invalid
 * input.
 */
bool boxrig_font_visual_step_sequence_advance(
    boxrig_font_visual_step_sequence *sequence,
    boxrig_font_zoom_direction direction);

/**
 * Reads the state from which the current visual-step sequence was created.
 *
 * @param sequence Sequence to inspect.
 * @param metrics_out Receives the reset target when it differs from current.
 * @return True when resetting would change the sequence, or false otherwise.
 */
bool boxrig_font_visual_step_sequence_peek_reset(
    const boxrig_font_visual_step_sequence *sequence,
    boxrig_font_effective_metrics *metrics_out);

/**
 * Returns a visual-step sequence to the state from which it was created.
 *
 * @param sequence Sequence to update.
 * @return True when the sequence changed, or false when already reset or for
 * invalid input.
 */
bool boxrig_font_visual_step_sequence_reset(
    boxrig_font_visual_step_sequence *sequence);

#ifdef __cplusplus
}
#endif

#endif
