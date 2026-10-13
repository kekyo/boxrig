#ifndef BOXRIG_TEST_SEARCH_TRANSFORM_PROTOTYPE_H
#define BOXRIG_TEST_SEARCH_TRANSFORM_PROTOTYPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Maximum source bytes accepted by the transform prototype. */
#define SEARCH_TRANSFORM_PROTOTYPE_SOURCE_HARD_LIMIT ((size_t)65536u)

/** Maximum transformed bytes retained by the transform prototype. */
#define SEARCH_TRANSFORM_PROTOTYPE_OUTPUT_HARD_LIMIT ((size_t)65536u)

/** Maximum source bytes transformed by one non-cancellable map call. */
#define SEARCH_TRANSFORM_PROTOTYPE_GRAPHEME_HARD_LIMIT ((size_t)4096u)

/** Transformation evaluated by the search prototype. */
typedef enum search_transform_prototype_mode {
    /** Unicode default case folding without normalization or locale tailoring. */
    SEARCH_TRANSFORM_PROTOTYPE_CASE_FOLD,
    /** Canonical normalization form C without compatibility mappings. */
    SEARCH_TRANSFORM_PROTOTYPE_NFC,
    /** Default case folding followed by canonical composition. */
    SEARCH_TRANSFORM_PROTOTYPE_CASE_FOLD_NFC
} search_transform_prototype_mode;

/** Result of constructing a transformed prototype text. */
typedef enum search_transform_prototype_status {
    /** The complete transformed text was created. */
    SEARCH_TRANSFORM_PROTOTYPE_OK,
    /** An input, mode, UTF-8 sequence, or grapheme boundary was invalid. */
    SEARCH_TRANSFORM_PROTOTYPE_INVALID,
    /** The transformed output would exceed the caller's strict limit. */
    SEARCH_TRANSFORM_PROTOTYPE_LIMIT,
    /** An allocation failed. */
    SEARCH_TRANSFORM_PROTOTYPE_NO_MEMORY
} search_transform_prototype_status;

/** Owned transformed bytes and original-grapheme boundary mapping. */
typedef struct search_transform_prototype_text {
    /** Transformed UTF-8 bytes. */
    uint8_t *bytes;
    /** Number of transformed UTF-8 bytes. */
    size_t byte_length;
    /** Original byte offsets for every source grapheme boundary. */
    size_t *source_offsets;
    /** Transformed byte offsets corresponding to source_offsets. */
    size_t *transformed_offsets;
    /** Number of entries in each boundary array. */
    size_t boundary_count;
    /** Allocated retained bytes, excluding utf8proc's temporary mapping. */
    size_t retained_bytes;
    /** Largest returned temporary mapping including its NUL terminator. */
    size_t largest_mapping_bytes;
} search_transform_prototype_text;

/** Maximum expansion observed while mapping one Unicode scalar. */
typedef struct search_transform_prototype_metrics {
    /** Number of Unicode scalar values inspected. */
    size_t scalar_count;
    /** Scalar value whose transformed UTF-8 result was largest. */
    uint32_t largest_scalar;
    /** UTF-8 length of largest_scalar before transformation. */
    size_t input_bytes;
    /** UTF-8 length of largest_scalar after transformation. */
    size_t output_bytes;
} search_transform_prototype_metrics;

/**
 * Transforms complete source graphemes independently and retains a boundary map.
 *
 * @param mode Unicode transformation to apply.
 * @param input Length-delimited UTF-8 source.
 * @param input_length Number of source bytes.
 * @param source_boundaries Strictly increasing byte boundaries including zero
 *        and input_length.
 * @param boundary_count Number of source_boundaries entries.
 * @param output_limit Strict maximum number of transformed bytes.
 * @param output Receives an owned all-or-nothing result and must be zeroed.
 * @return Prototype status.
 */
search_transform_prototype_status search_transform_prototype_create(
    search_transform_prototype_mode mode,
    const uint8_t *input,
    size_t input_length,
    const size_t *source_boundaries,
    size_t boundary_count,
    size_t output_limit,
    search_transform_prototype_text *output);

/**
 * Finds the first exact transformed match whose ends are source boundaries.
 *
 * @param text Transformed document text.
 * @param pattern Non-empty transformed pattern.
 * @param source_begin Receives the original inclusive byte boundary.
 * @param source_end Receives the original exclusive byte boundary.
 * @return true when a boundary-aligned transformed match exists.
 */
bool search_transform_prototype_find(
    const search_transform_prototype_text *text,
    const search_transform_prototype_text *pattern,
    size_t *source_begin,
    size_t *source_end);

/**
 * Measures the largest one-scalar UTF-8 expansion for one transformation.
 *
 * @param mode Unicode transformation to inspect.
 * @param metrics Receives measurements for every Unicode scalar value.
 * @return Prototype status.
 */
search_transform_prototype_status search_transform_prototype_measure_scalars(
    search_transform_prototype_mode mode,
    search_transform_prototype_metrics *metrics);

/**
 * Releases a transformed prototype text.
 *
 * @param text Text to release. NULL is accepted.
 */
void search_transform_prototype_destroy(
    search_transform_prototype_text *text);

#endif
