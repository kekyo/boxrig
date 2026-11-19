/* Test-only observation of terminal changes and renderer work in the Linux samples. */
#include "../../src/opengl/renderer.h"

#include <pango/pango.h>
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct cell_snapshot {
    uint64_t text;
    uint64_t style;
    uint32_t font_style;
    bool boundary;
    bool digit;
    bool changed;
    bool shaped;
} cell_snapshot;

typedef struct row_changes {
    size_t changed, text, digits, boundaries;
    bool shaped;
} row_changes;

static boxrig_ogl_renderer *observed_renderer;
static boxrig_renderer_lifecycle *observed_lifecycle;
static FILE *output;
static cell_snapshot *previous;
static row_changes *changes;
static size_t previous_rows, previous_columns, frame_number;
static size_t sync_ends, matched_bytes;
static boxrig_ogl_row_shape *active_shape;
static size_t active_row;

boxrig_status __real_boxrig_terminal_feed(boxrig_terminal *, const uint8_t *, size_t, size_t *);
boxrig_status __wrap_boxrig_terminal_feed(boxrig_terminal *terminal,
    const uint8_t *bytes, size_t count, size_t *consumed)
{
    static const char end[] = "\033[?2026l";
    boxrig_status status = __real_boxrig_terminal_feed(terminal, bytes, count, consumed);
    if (consumed != NULL) for (size_t i = 0; i < *consumed; i++) {
        if (bytes[i] == (uint8_t)end[matched_bytes]) matched_bytes++;
        else matched_bytes = bytes[i] == (uint8_t)end[0] ? 1u : 0u;
        if (matched_bytes == sizeof(end) - 1) {
            matched_bytes = 0;
            sync_ends++;
        }
    }
    return status;
}

static uint64_t append_byte(uint64_t hash, unsigned int byte)
{
    return (hash ^ (byte & 255u)) * UINT64_C(1099511628211);
}

static uint64_t append_number(uint64_t hash, uint32_t number)
{
    for (unsigned int shift = 0; shift < 32; shift += 8)
        hash = append_byte(hash, number >> shift);
    return hash;
}

static uint32_t color_value(boxrig_color color)
{
    uint32_t value = (uint32_t)color.kind << 24;
    if (color.kind == BOXRIG_COLOR_INDEXED) return value | color.index;
    if (color.kind == BOXRIG_COLOR_RGB)
        return value | ((uint32_t)color.red << 16) | ((uint32_t)color.green << 8) | color.blue;
    return value;
}

static cell_snapshot snapshot(boxrig_cell_view cell)
{
    cell_snapshot value = {0};
    value.text = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < cell.text_length; i++)
        value.text = append_byte(value.text, (uint8_t)cell.text[i]);
    value.text = append_number(value.text, cell.column_width);
    value.text = append_byte(value.text, cell.continuation);
    value.text = append_byte(value.text, cell.font_dependent);
    value.font_style = cell.attributes & (BOXRIG_CELL_BOLD | BOXRIG_CELL_FAINT | BOXRIG_CELL_ITALIC);
    value.digit = cell.text_length == 1 && cell.text[0] >= '0' && cell.text[0] <= '9';
    value.style = append_number(UINT64_C(1469598103934665603), cell.attributes);
    value.style = append_number(value.style, cell.underline_style);
    value.style = append_number(value.style, cell.protected_cell);
    value.style = append_number(value.style, color_value(cell.foreground));
    value.style = append_number(value.style, color_value(cell.background));
    value.style = append_number(value.style, color_value(cell.underline_color));
    value.style = append_number(value.style, cell.hyperlink_handle);
    return value;
}

static void release_probe(void)
{
    if (output != NULL) fclose(output);
    free(previous);
    free(changes);
}

boxrig_renderer_status __real_boxrig_ogl_renderer_lifecycle_create(
    boxrig_ogl_renderer *, const boxrig_renderer_target *, boxrig_renderer_lifecycle **);
boxrig_renderer_status __wrap_boxrig_ogl_renderer_lifecycle_create(
    boxrig_ogl_renderer *renderer, const boxrig_renderer_target *target, boxrig_renderer_lifecycle **lifecycle)
{
    boxrig_renderer_status status = __real_boxrig_ogl_renderer_lifecycle_create(renderer, target, lifecycle);
    const char *path = getenv("BOXRIG_UPDATE_WORK_PATH");
    if (status == BOXRIG_RENDERER_OK && path != NULL) {
        observed_renderer = renderer;
        observed_lifecycle = *lifecycle;
        if (output == NULL) {
            output = fopen(path, "w");
            assert(output != NULL && atexit(release_probe) == 0);
            fputs("frame\tsync_ends\tbackground\trows\tcolumns\tchanged\ttext\tdigits\tshape_rows\tshape_columns\t"
                "unchanged_in_shaped\tdigit_only_rows\ttext_rows\tboundary_rows\tother_rows\traster_columns\n", output);
        }
    }
    return status;
}

int __real_boxrig_ogl_text_provider_shape_row(
    boxrig_ogl_text_provider *, const boxrig_terminal *, size_t, boxrig_ogl_row_shape *);
int __wrap_boxrig_ogl_text_provider_shape_row(
    boxrig_ogl_text_provider *provider, const boxrig_terminal *terminal, size_t row,
    boxrig_ogl_row_shape *shape)
{
    active_row = row;
    active_shape = shape;
    int result = __real_boxrig_ogl_text_provider_shape_row(provider, terminal, row, shape);
    active_shape = NULL;
    if (result == 0 && changes != NULL && row < previous_rows) changes[row].shaped = true;
    return result;
}

void __real_pango_shape_item(PangoItem *, const char *, int, PangoLogAttr *, PangoGlyphString *, PangoShapeFlags);
void __wrap_pango_shape_item(PangoItem *item, const char *text, int length, PangoLogAttr *attrs,
    PangoGlyphString *glyphs, PangoShapeFlags flags)
{
    __real_pango_shape_item(item, text, length, attrs, glyphs, flags);
    if (active_shape == NULL || previous == NULL || active_row >= previous_rows) return;
    for (size_t i = 0; i < active_shape->cluster_count; i++) {
        const boxrig_ogl_logical_cluster *cluster = &active_shape->clusters[i];
        if (cluster->text_offset >= (size_t)item->offset + (size_t)item->length ||
            cluster->text_offset + cluster->text_length <= (size_t)item->offset) continue;
        assert(cluster->logical_column + cluster->column_count <= previous_columns);
        for (size_t c = cluster->logical_column; c < cluster->logical_column + cluster->column_count; c++)
            previous[active_row * previous_columns + c].shaped = true;
    }
}

boxrig_renderer_status __real_boxrig_renderer_lifecycle_prepare_frame(boxrig_renderer_lifecycle *,
    boxrig_ui_view *, boxrig_composition *, uint64_t, boxrig_renderer_frame_token *);
boxrig_renderer_status __wrap_boxrig_renderer_lifecycle_prepare_frame(boxrig_renderer_lifecycle *lifecycle,
    boxrig_ui_view *view, boxrig_composition *composition, uint64_t generation, boxrig_renderer_frame_token *token)
{
    if (lifecycle != observed_lifecycle || output == NULL)
        return __real_boxrig_renderer_lifecycle_prepare_frame(lifecycle, view, composition, generation, token);
    boxrig_terminal *terminal = boxrig_ui_model_terminal(boxrig_ui_view_model(view));
    size_t rows, columns;
    boxrig_terminal_size(terminal, &rows, &columns);
    if (rows != previous_rows || columns != previous_columns) {
        free(previous);
        free(changes);
        previous = calloc(rows * columns, sizeof(*previous));
        changes = calloc(rows, sizeof(*changes));
        assert(previous != NULL && changes != NULL);
        previous_rows = rows;
        previous_columns = columns;
    }
    memset(changes, 0, rows * sizeof(*changes));
    uint32_t background = 0;
    for (size_t row = 0; row < rows; row++) {
        uint64_t last_style = 0;
        for (size_t column = 0; column < columns; column++) {
            boxrig_cell_view cell;
            assert(boxrig_terminal_get_cell(terminal, row, column, &cell) == BOXRIG_STATUS_OK);
            if (row == 0 && column == 0) background = color_value(cell.background);
            cell_snapshot current = snapshot(cell);
            if (!cell.continuation) {
                current.boundary = column != 0 && current.style != last_style;
                last_style = current.style;
            }
            cell_snapshot old = previous[row * columns + column];
            bool text_changed = current.text != old.text || current.font_style != old.font_style;
            current.changed = text_changed || current.style != old.style;
            changes[row].changed += current.changed;
            changes[row].boundaries += current.boundary != old.boundary;
            changes[row].text += text_changed;
            changes[row].digits += text_changed && current.digit && old.digit && current.style == old.style;
            previous[row * columns + column] = current;
        }
    }
    boxrig_ogl_render_statistics before, after;
    assert(boxrig_ogl_renderer_get_render_statistics(observed_renderer, &before) == 0);
    boxrig_renderer_status status = __real_boxrig_renderer_lifecycle_prepare_frame(
        lifecycle, view, composition, generation, token);
    assert(boxrig_ogl_renderer_get_render_statistics(observed_renderer, &after) == 0);
    if (status != BOXRIG_RENDERER_OK) return status;
    size_t changed = 0, text = 0, digits = 0, unchanged = 0, digit_rows = 0;
    size_t shaped = 0, shaped_columns = 0, text_rows = 0, boundary_rows = 0, other_rows = 0;
    for (size_t row = 0; row < rows; row++) {
        const row_changes *value = &changes[row];
        changed += value->changed;
        text += value->text;
        digits += value->digits;
        if (!value->shaped) continue;
        shaped++;
        for (size_t c = 0; c < columns; c++) {
            const cell_snapshot *cell = &previous[row * columns + c];
            shaped_columns += cell->shaped;
            unchanged += cell->shaped && !cell->changed;
        }
        digit_rows += value->digits > 0 && value->digits == value->text && value->boundaries == 0;
        if (value->text > 0) text_rows++;
        else if (value->boundaries > 0) boundary_rows++;
        else other_rows++;
    }
    fprintf(output, "%zu\t%zu\t%08" PRIx32 "\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\n",
        frame_number++, sync_ends, background, rows, columns, changed, text, digits, shaped, shaped_columns, unchanged,
        digit_rows, text_rows, boundary_rows, other_rows, after.cell_raster_count - before.cell_raster_count);
    fflush(output);
    return status;
}
