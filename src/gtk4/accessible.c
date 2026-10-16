/* boxrig - Under MIT. */
#include "accessible.h"

static const boxrig_ui_accessible_snapshot *snapshot(GtkAccessibleText *text)
{
    boxrig_gtk_accessible_state *state = boxrig_gtk4_accessible_state(BOXRIG_GTK4_TERMINAL_WIDGET(text));
    return state->detached ? NULL : state->snapshot;
}

static GBytes *contents(GtkAccessibleText *text, unsigned int start, unsigned int end)
{
    const boxrig_ui_accessible_snapshot *value = snapshot(text);
    if (value == NULL || start > value->character_count || end < start)
        return g_bytes_new_static("", 0);
    if (end > value->character_count) end = (unsigned int)value->character_count;
    size_t first = value->characters[start].byte_offset;
    size_t last = value->characters[end].byte_offset;
    return g_bytes_new(value->text + first, last - first);
}

static GBytes *contents_at(GtkAccessibleText *text, unsigned int offset,
    GtkAccessibleTextGranularity granularity, unsigned int *start, unsigned int *end)
{
    *start = *end = 0;
    boxrig_ui_accessible_range range;
    if (!boxrig_gtk_accessible_text_range(snapshot(text), offset, (unsigned int)granularity, &range))
        return g_bytes_new_static("", 0);
    *start = (unsigned int)range.start;
    *end = (unsigned int)range.end;
    return contents(text, *start, *end);
}

static unsigned int caret_position(GtkAccessibleText *text)
{
    const boxrig_ui_accessible_snapshot *value = snapshot(text);
    return value == NULL ? 0 : (unsigned int)value->caret;
}

static gboolean selection(GtkAccessibleText *text, gsize *count, GtkAccessibleTextRange **ranges)
{
    *count = 0;
    if (ranges != NULL) *ranges = NULL;
    const boxrig_ui_accessible_snapshot *value = snapshot(text);
    if (value == NULL || value->selection_count == 0) return FALSE;
    *count = value->selection_count;
    if (ranges != NULL) {
        *ranges = g_new(GtkAccessibleTextRange, *count);
        for (gsize i = 0; i < *count; i++) {
            const boxrig_ui_accessible_range *source = &value->selections[i];
            (*ranges)[i] = (GtkAccessibleTextRange){source->start, source->end - source->start};
        }
    }
    return TRUE;
}

static void default_attributes(GtkAccessibleText *text, char ***names, char ***values)
{
    (void)text;
    if (names != NULL) {
        *names = g_new0(char *, 3);
        (*names)[0] = g_strdup("editable");
        (*names)[1] = g_strdup("wrap-mode");
    }
    if (values != NULL) {
        *values = g_new0(char *, 3);
        (*values)[0] = g_strdup("false");
        (*values)[1] = g_strdup("char");
    }
}

static gboolean attributes(GtkAccessibleText *text, unsigned int offset, gsize *count,
    GtkAccessibleTextRange **ranges, char ***names, char ***values)
{
    *count = 0;
    if (ranges != NULL) *ranges = NULL;
    if (names != NULL) *names = NULL;
    if (values != NULL) *values = NULL;
    const boxrig_ui_accessible_snapshot *value = snapshot(text);
    if (value == NULL || offset >= value->character_count) return FALSE;
    const boxrig_ui_accessible_character *character = &value->characters[offset];
    size_t start = offset, end = offset + 1;
    while (start > 0 && boxrig_gtk_accessible_same_attributes(character, &value->characters[start - 1])) start--;
    while (end < value->character_count && boxrig_gtk_accessible_same_attributes(character, &value->characters[end])) end++;
    const char *attribute_names[] = {
        GTK_ACCESSIBLE_ATTRIBUTE_WEIGHT, GTK_ACCESSIBLE_ATTRIBUTE_STYLE,
        GTK_ACCESSIBLE_ATTRIBUTE_UNDERLINE, GTK_ACCESSIBLE_ATTRIBUTE_STRIKETHROUGH,
        "invisible", GTK_ACCESSIBLE_ATTRIBUTE_FOREGROUND, GTK_ACCESSIBLE_ATTRIBUTE_BACKGROUND
    };
    *count = G_N_ELEMENTS(attribute_names);
    if (ranges != NULL) {
        *ranges = g_new(GtkAccessibleTextRange, *count);
        for (gsize i = 0; i < *count; i++) (*ranges)[i] = (GtkAccessibleTextRange){start, end - start};
    }
    if (names != NULL) {
        *names = g_new0(char *, *count + 1);
        for (gsize i = 0; i < *count; i++) (*names)[i] = g_strdup(attribute_names[i]);
    }
    if (values != NULL) {
        *values = g_new0(char *, *count + 1);
        (*values)[0] = g_strdup(character->attributes & BOXRIG_CELL_BOLD ? "700" : "400");
        (*values)[1] = g_strdup(character->attributes & BOXRIG_CELL_ITALIC ?
            GTK_ACCESSIBLE_ATTRIBUTE_STYLE_ITALIC : GTK_ACCESSIBLE_ATTRIBUTE_STYLE_NORMAL);
        (*values)[2] = g_strdup(character->underline == BOXRIG_UNDERLINE_NONE ? GTK_ACCESSIBLE_ATTRIBUTE_UNDERLINE_NONE :
            character->underline == BOXRIG_UNDERLINE_DOUBLE ? GTK_ACCESSIBLE_ATTRIBUTE_UNDERLINE_DOUBLE :
            GTK_ACCESSIBLE_ATTRIBUTE_UNDERLINE_SINGLE);
        (*values)[3] = g_strdup(character->attributes & BOXRIG_CELL_STRIKETHROUGH ? "true" : "false");
        (*values)[4] = g_strdup(character->attributes & BOXRIG_CELL_CONCEALED ? "true" : "false");
        /* GTK's public attribute contract uses 8-bit RGB, unlike ATK's
         * 16-bit color components. */
        (*values)[5] = g_strdup_printf("%u,%u,%u", character->foreground.red,
            character->foreground.green, character->foreground.blue);
        (*values)[6] = g_strdup_printf("%u,%u,%u", character->background.red,
            character->background.green, character->background.blue);
    }
    return TRUE;
}

void boxrig_gtk4_accessible_text_init(GtkAccessibleTextInterface *interface)
{
    interface->get_contents = contents;
    interface->get_contents_at = contents_at;
    interface->get_caret_position = caret_position;
    interface->get_selection = selection;
    interface->get_attributes = attributes;
    interface->get_default_attributes = default_attributes;
    /* Keep the 4.14 interface contract. Later geometry and editing vfuncs
     * are not required for the verified AT-SPI/Orca text path. In particular,
     * terminal cursor movement must not synthesize child-process input. */
}

static void notify(boxrig_gtk_accessible_event event,
    const boxrig_ui_accessible_change *change, void *context)
{
    GtkAccessibleText *text = context;
    switch (event) {
    case BOXRIG_GTK_ACCESSIBLE_REMOVE:
        gtk_accessible_text_update_contents(text, GTK_ACCESSIBLE_TEXT_CONTENT_CHANGE_REMOVE,
            (unsigned int)change->start, (unsigned int)(change->start + change->removed));
        break;
    case BOXRIG_GTK_ACCESSIBLE_INSERT:
        gtk_accessible_text_update_contents(text, GTK_ACCESSIBLE_TEXT_CONTENT_CHANGE_INSERT,
            (unsigned int)change->start, (unsigned int)(change->start + change->inserted));
        break;
    case BOXRIG_GTK_ACCESSIBLE_CARET:
        gtk_accessible_text_update_caret_position(text);
        break;
    case BOXRIG_GTK_ACCESSIBLE_SELECTION:
        gtk_accessible_text_update_selection_bound(text);
        break;
    case BOXRIG_GTK_ACCESSIBLE_ATTRIBUTES:
        /* GTK 4.14 has no public attribute-only update notification. The
         * newly published attributes remain queryable without fake edits. */
        break;
    }
}

boxrig_ui_status boxrig_gtk4_accessible_update(BoxrigGtk4TerminalWidget *widget)
{
    return boxrig_gtk_accessible_publish(boxrig_gtk4_accessible_state(widget),
        boxrig_gtk4_terminal_widget_view(widget),
        boxrig_gtk4_terminal_widget_get_accessibility_enabled(widget), notify, GTK_ACCESSIBLE_TEXT(widget));
}
