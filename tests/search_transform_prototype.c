#include "search_transform_prototype.h"

#include "utf8proc.h"

#include <stdlib.h>
#include <string.h>

static bool multiply_size(size_t left, size_t right, size_t *result)
{
    if (left != 0u && right > SIZE_MAX / left) {
        return false;
    }
    *result = left * right;
    return true;
}

static bool add_size(size_t left, size_t right, size_t *result)
{
    if (right > SIZE_MAX - left) {
        return false;
    }
    *result = left + right;
    return true;
}

static bool output_is_zeroed(const search_transform_prototype_text *output)
{
    return output != NULL && output->bytes == NULL &&
           output->byte_length == 0u && output->source_offsets == NULL &&
           output->transformed_offsets == NULL &&
           output->boundary_count == 0u && output->retained_bytes == 0u &&
           output->largest_mapping_bytes == 0u;
}

static bool transform_options(
    search_transform_prototype_mode mode,
    utf8proc_option_t *options)
{
    switch (mode) {
    case SEARCH_TRANSFORM_PROTOTYPE_CASE_FOLD:
        *options = (utf8proc_option_t)(UTF8PROC_STABLE |
                                      UTF8PROC_CASEFOLD);
        return true;
    case SEARCH_TRANSFORM_PROTOTYPE_NFC:
        *options = (utf8proc_option_t)(UTF8PROC_STABLE | UTF8PROC_COMPOSE);
        return true;
    case SEARCH_TRANSFORM_PROTOTYPE_CASE_FOLD_NFC:
        *options = (utf8proc_option_t)(UTF8PROC_STABLE |
                                      UTF8PROC_CASEFOLD |
                                      UTF8PROC_COMPOSE);
        return true;
    default:
        return false;
    }
}

static search_transform_prototype_status map_error_status(
    utf8proc_ssize_t result)
{
    return result == UTF8PROC_ERROR_NOMEM
               ? SEARCH_TRANSFORM_PROTOTYPE_NO_MEMORY
               : SEARCH_TRANSFORM_PROTOTYPE_INVALID;
}

static bool source_boundary_at(
    const search_transform_prototype_text *text,
    size_t transformed_offset,
    size_t *source_offset)
{
    size_t low = 0u;
    size_t high = text->boundary_count;

    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        size_t candidate = text->transformed_offsets[middle];

        if (candidate < transformed_offset) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    if (low >= text->boundary_count ||
        text->transformed_offsets[low] != transformed_offset) {
        return false;
    }
    *source_offset = text->source_offsets[low];
    return true;
}

search_transform_prototype_status search_transform_prototype_create(
    search_transform_prototype_mode mode,
    const uint8_t *input,
    size_t input_length,
    const size_t *source_boundaries,
    size_t boundary_count,
    size_t output_limit,
    search_transform_prototype_text *output)
{
    utf8proc_option_t options;
    uint8_t *bytes = NULL;
    size_t *source_offsets = NULL;
    size_t *transformed_offsets = NULL;
    size_t array_bytes;
    size_t retained_bytes;
    size_t output_length = 0u;
    size_t largest_mapping_bytes = 0u;
    size_t index;
    search_transform_prototype_status status =
        SEARCH_TRANSFORM_PROTOTYPE_INVALID;

    if (!output_is_zeroed(output) || !transform_options(mode, &options) ||
        input == NULL || input_length == 0u || source_boundaries == NULL ||
        boundary_count < 2u || source_boundaries[0] != 0u ||
        source_boundaries[boundary_count - 1u] != input_length ||
        input_length > SEARCH_TRANSFORM_PROTOTYPE_SOURCE_HARD_LIMIT ||
        output_limit == 0u ||
        output_limit > SEARCH_TRANSFORM_PROTOTYPE_OUTPUT_HARD_LIMIT ||
        output_limit == SIZE_MAX ||
        !multiply_size(boundary_count, sizeof(*source_offsets),
                       &array_bytes) ||
        !add_size(output_limit + 1u, array_bytes, &retained_bytes) ||
        !add_size(retained_bytes, array_bytes, &retained_bytes)) {
        return SEARCH_TRANSFORM_PROTOTYPE_INVALID;
    }
    for (index = 1u; index < boundary_count; ++index) {
        size_t previous = source_boundaries[index - 1u];
        size_t current = source_boundaries[index];

        if (current <= previous || current > input_length ||
            current - previous >
                SEARCH_TRANSFORM_PROTOTYPE_GRAPHEME_HARD_LIMIT) {
            return SEARCH_TRANSFORM_PROTOTYPE_INVALID;
        }
    }

    bytes = malloc(output_limit + 1u);
    source_offsets = malloc(array_bytes);
    transformed_offsets = malloc(array_bytes);
    if (bytes == NULL || source_offsets == NULL ||
        transformed_offsets == NULL) {
        status = SEARCH_TRANSFORM_PROTOTYPE_NO_MEMORY;
        goto fail;
    }
    memcpy(source_offsets, source_boundaries, array_bytes);
    transformed_offsets[0] = 0u;

    for (index = 1u; index < boundary_count; ++index) {
        size_t source_begin = source_boundaries[index - 1u];
        size_t source_length = source_boundaries[index] - source_begin;
        utf8proc_uint8_t *mapping = NULL;
        utf8proc_ssize_t mapped = utf8proc_map(
            (const utf8proc_uint8_t *)input + source_begin,
            (utf8proc_ssize_t)source_length,
            &mapping,
            options);
        size_t mapped_length;

        if (mapped < 0) {
            status = map_error_status(mapped);
            free(mapping);
            goto fail;
        }
        mapped_length = (size_t)mapped;
        if (mapped_length > output_limit - output_length) {
            free(mapping);
            status = SEARCH_TRANSFORM_PROTOTYPE_LIMIT;
            goto fail;
        }
        memcpy(bytes + output_length, mapping, mapped_length);
        output_length += mapped_length;
        transformed_offsets[index] = output_length;
        if (mapped_length + 1u > largest_mapping_bytes) {
            largest_mapping_bytes = mapped_length + 1u;
        }
        free(mapping);
    }
    bytes[output_length] = 0u;

    output->bytes = bytes;
    output->byte_length = output_length;
    output->source_offsets = source_offsets;
    output->transformed_offsets = transformed_offsets;
    output->boundary_count = boundary_count;
    output->retained_bytes = retained_bytes;
    output->largest_mapping_bytes = largest_mapping_bytes;
    return SEARCH_TRANSFORM_PROTOTYPE_OK;

fail:
    free(transformed_offsets);
    free(source_offsets);
    free(bytes);
    return status;
}

bool search_transform_prototype_find(
    const search_transform_prototype_text *text,
    const search_transform_prototype_text *pattern,
    size_t *source_begin,
    size_t *source_end)
{
    size_t offset;

    if (text == NULL || pattern == NULL || source_begin == NULL ||
        source_end == NULL || text->bytes == NULL || pattern->bytes == NULL ||
        text->source_offsets == NULL || text->transformed_offsets == NULL ||
        text->boundary_count < 2u || pattern->byte_length == 0u ||
        pattern->byte_length > text->byte_length) {
        return false;
    }

    for (offset = 0u;
         offset <= text->byte_length - pattern->byte_length;
         ++offset) {
        size_t candidate_begin;
        size_t candidate_end;

        if (memcmp(text->bytes + offset,
                   pattern->bytes,
                   pattern->byte_length) != 0 ||
            !source_boundary_at(text, offset, &candidate_begin) ||
            !source_boundary_at(text,
                                offset + pattern->byte_length,
                                &candidate_end)) {
            continue;
        }
        *source_begin = candidate_begin;
        *source_end = candidate_end;
        return true;
    }
    return false;
}

search_transform_prototype_status search_transform_prototype_measure_scalars(
    search_transform_prototype_mode mode,
    search_transform_prototype_metrics *metrics)
{
    utf8proc_option_t options;
    search_transform_prototype_metrics measured = {0};
    uint32_t scalar;

    if (metrics == NULL || !transform_options(mode, &options)) {
        return SEARCH_TRANSFORM_PROTOTYPE_INVALID;
    }

    for (scalar = 0u; scalar < 0x110000u; ++scalar) {
        utf8proc_uint8_t input[4];
        utf8proc_uint8_t *mapping = NULL;
        utf8proc_ssize_t input_length;
        utf8proc_ssize_t mapped;

        if (scalar >= 0xD800u && scalar <= 0xDFFFu) {
            continue;
        }
        input_length = utf8proc_encode_char(
            (utf8proc_int32_t)scalar, input);
        if (input_length <= 0) {
            return SEARCH_TRANSFORM_PROTOTYPE_INVALID;
        }
        mapped = utf8proc_map(input, input_length, &mapping, options);
        if (mapped < 0) {
            search_transform_prototype_status status =
                map_error_status(mapped);
            free(mapping);
            return status;
        }
        ++measured.scalar_count;
        if ((size_t)mapped > measured.output_bytes) {
            measured.largest_scalar = scalar;
            measured.input_bytes = (size_t)input_length;
            measured.output_bytes = (size_t)mapped;
        }
        free(mapping);
    }

    *metrics = measured;
    return SEARCH_TRANSFORM_PROTOTYPE_OK;
}

void search_transform_prototype_destroy(
    search_transform_prototype_text *text)
{
    if (text == NULL) {
        return;
    }
    free(text->transformed_offsets);
    free(text->source_offsets);
    free(text->bytes);
    memset(text, 0, sizeof(*text));
}
