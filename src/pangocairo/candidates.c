/* boxrig - Under MIT. */
#include "candidates.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

void boxrig_pango_candidates_dispose(boxrig_pango_candidate_frame *frame)
{
    for (size_t i = 0; i < BOXRIG_UI_CANDIDATE_PAGE_MAXIMUM; i++)
        if (frame->items[i] != NULL) g_object_unref(frame->items[i]);
    if (frame->auxiliary != NULL) g_object_unref(frame->auxiliary);
    memset(frame, 0, sizeof(*frame));
}

static PangoLayout *create_layout(PangoContext *context, const PangoFontDescription *font,
    const char *features, boxrig_font_fallback_policy fallback, const char *text, bool single)
{
    PangoLayout *layout = pango_layout_new(context);
    if (layout == NULL) return NULL;
    pango_layout_set_font_description(layout, font);
    pango_layout_set_single_paragraph_mode(layout, single);
    pango_layout_set_text(layout, text, -1);
    PangoAttrList *attributes = pango_attr_list_new();
    if (features != NULL) pango_attr_list_insert(attributes, pango_attr_font_features_new(features));
    if (fallback != BOXRIG_FONT_FALLBACK_SYSTEM)
        pango_attr_list_insert(attributes, pango_attr_fallback_new(FALSE));
    pango_layout_set_attributes(layout, attributes);
    pango_attr_list_unref(attributes);
    return layout;
}

static char *auxiliary_lines(const char *source)
{
    size_t length = strlen(source);
    char *text = malloc(length * 3 + 1);
    if (text == NULL) return NULL;
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char a = (unsigned char)source[i];
        bool newline = a == '\r' || a == '\n';
        if (a == '\r' && i + 1 < length && source[i + 1] == '\n') i++;
        if (a == 0xc2 && i + 1 < length && (unsigned char)source[i + 1] == 0x85) {
            i++; newline = true;
        }
        if (a == 0xe2 && i + 2 < length && (unsigned char)source[i + 1] == 0x80 &&
            (unsigned char)source[i + 2] == 0xa9) { i += 2; newline = true; }
        /* Pango's negative height is per paragraph. A line separator retains
         * explicit line breaks in one paragraph, giving a true two-line cap. */
        if (newline) { memcpy(text + used, "\xe2\x80\xa8", 3); used += 3; }
        else text[used++] = source[i];
    }
    text[used] = 0;
    return text;
}

static void constrain_layout(PangoLayout *layout, int width, int lines)
{
    if (width < 1) width = 1;
    if (width > INT_MAX / PANGO_SCALE) width = INT_MAX / PANGO_SCALE;
    pango_layout_set_width(layout, width * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_height(layout, -lines);
}

int boxrig_pango_candidates_prepare(PangoContext *context,
    const PangoFontDescription *font, const char *features, boxrig_font_fallback_policy fallback,
    const boxrig_ui_candidate_view *candidates, boxrig_ui_candidate_measurement m,
    boxrig_pango_candidate_frame *frame)
{
    memset(frame, 0, sizeof(*frame));
    frame->layout.revision = candidates->revision;
    const boxrig_ui_candidate_snapshot *s = &candidates->snapshot;
    if (s->count == 0 || (!s->visible && !s->auxiliary_visible)) return 0;
    frame->page = s->cursor / s->page_size * s->page_size;
    frame->selected = s->cursor;
    frame->cursor_visible = s->cursor_visible;
    frame->padding = m.cell_width / 2 > 2 ? m.cell_width / 2 : 2;
    size_t count = s->visible ? s->count - frame->page : 0;
    if (count > s->page_size) count = s->page_size;
    for (size_t i = 0; i < count; i++) {
        const char *item = s->items[frame->page + i];
        const char *label = s->labels == NULL ? NULL : s->labels[i];
        size_t label_length = label == NULL ? 0 : strlen(label);
        size_t item_length = strlen(item);
        char *text = malloc(label_length + item_length + 3);
        if (text == NULL) goto failure;
        size_t offset = 0;
        if (label_length != 0) {
            memcpy(text, label, label_length); offset = label_length;
            text[offset++] = ' '; text[offset++] = ' ';
        }
        memcpy(text + offset, item, item_length + 1);
        frame->items[i] = create_layout(context, font, features, fallback, text, true);
        free(text);
        if (frame->items[i] == NULL) goto failure;
        int height;
        pango_layout_get_pixel_size(frame->items[i], &m.items[i], &height);
        if (height > m.line_height) m.line_height = height;
    }
    if (s->auxiliary_visible && s->auxiliary != NULL && s->auxiliary[0] != 0) {
        char *text = auxiliary_lines(s->auxiliary);
        if (text == NULL) goto failure;
        frame->auxiliary = create_layout(context, font, features, fallback, text, false);
        free(text);
        if (frame->auxiliary == NULL) goto failure;
        constrain_layout(frame->auxiliary, m.width - 2 - frame->padding * 2, 2);
        int height;
        pango_layout_get_pixel_size(frame->auxiliary, &m.auxiliary_width, &height);
        int lines = pango_layout_get_line_count(frame->auxiliary);
        m.auxiliary_lines = lines > 2 ? 2 : lines;
        int line_height = lines == 0 ? 0 : (height + lines - 1) / lines;
        if (line_height > m.line_height) m.line_height = line_height;
    }
    if (boxrig_ui_candidates_place(candidates, &m, &frame->layout) != BOXRIG_UI_OK) goto failure;
    for (size_t i = 0; i < frame->layout.count; i++) {
        size_t source = frame->layout.first - frame->page + i;
        constrain_layout(frame->items[source], frame->layout.items[i].width - frame->padding * 2, 1);
    }
    if (frame->auxiliary != NULL && frame->layout.auxiliary.height > 0) {
        int lines = (frame->layout.auxiliary.height - frame->padding * 2) / m.line_height;
        constrain_layout(frame->auxiliary, frame->layout.auxiliary.width - frame->padding * 2, lines < 1 ? 1 : lines);
    }
    return 0;
failure:
    boxrig_pango_candidates_dispose(frame);
    return -1;
}

static void source_pixel(cairo_t *cr, uint32_t pixel)
{
    cairo_set_source_rgb(cr, ((pixel >> 16) & 255) / 255.0,
        ((pixel >> 8) & 255) / 255.0, (pixel & 255) / 255.0);
}

static void draw_text(cairo_t *cr, PangoLayout *text, boxrig_ui_candidate_rectangle rectangle,
    int padding, uint32_t color)
{
    if (text == NULL || rectangle.width <= 0 || rectangle.height <= 0) return;
    cairo_save(cr);
    cairo_rectangle(cr, rectangle.x, rectangle.y, rectangle.width, rectangle.height);
    cairo_clip(cr);
    source_pixel(cr, color);
    cairo_move_to(cr, rectangle.x + padding, rectangle.y + padding);
    pango_cairo_show_layout(cr, text);
    cairo_restore(cr);
}

int boxrig_pango_candidates_draw(cairo_t *cr, const boxrig_pango_candidate_frame *frame,
    uint32_t foreground, uint32_t background)
{
    boxrig_ui_candidate_rectangle panel = frame->layout.bounds;
    if (panel.width <= 0 || panel.height <= 0) return 0;
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_rectangle(cr, panel.x, panel.y, panel.width, panel.height);
    cairo_clip(cr);
    source_pixel(cr, foreground);
    cairo_paint(cr);
    source_pixel(cr, background);
    cairo_rectangle(cr, panel.x + 1, panel.y + 1, panel.width - 2, panel.height - 2);
    cairo_fill(cr);
    for (size_t i = 0; i < frame->layout.count; i++) {
        boxrig_ui_candidate_rectangle item = frame->layout.items[i];
        bool selected = frame->cursor_visible && frame->layout.first + i == frame->selected;
        if (selected) {
            source_pixel(cr, foreground);
            cairo_rectangle(cr, item.x, item.y, item.width, item.height);
            cairo_fill(cr);
        }
        draw_text(cr, frame->items[frame->layout.first - frame->page + i], item,
            frame->padding, selected ? background : foreground);
    }
    draw_text(cr, frame->auxiliary, frame->layout.auxiliary, frame->padding, foreground);
    /* Border markers indicate clipped entries without changing the engine's
     * page, labels, or selection. They do not create clickable controls. */
    source_pixel(cr, foreground);
    double center = panel.x + panel.width / 2.0;
    if (frame->layout.hidden_before) {
        cairo_move_to(cr, center, panel.y);
        cairo_line_to(cr, center - frame->padding, panel.y + frame->padding);
        cairo_line_to(cr, center + frame->padding, panel.y + frame->padding);
        cairo_close_path(cr); cairo_fill(cr);
    }
    if (frame->layout.hidden_after) {
        cairo_move_to(cr, center, panel.y + panel.height);
        cairo_line_to(cr, center - frame->padding, panel.y + panel.height - frame->padding);
        cairo_line_to(cr, center + frame->padding, panel.y + panel.height - frame->padding);
        cairo_close_path(cr); cairo_fill(cr);
    }
    cairo_restore(cr);
    return cairo_status(cr) == CAIRO_STATUS_SUCCESS ? 0 : -1;
}
