/* boxrig - Under MIT. */
#include "accessible.h"
#include "../gtk/accessible_text.h"

#include <gtk/gtk-a11y.h>
#include <limits.h>
#include <string.h>

typedef struct BoxrigGtk3TerminalAccessible {
    GtkContainerAccessible parent_instance;
    boxrig_gtk_accessible_state text;
    bool busy;
} BoxrigGtk3TerminalAccessible;

typedef struct BoxrigGtk3TerminalAccessibleClass {
    GtkContainerAccessibleClass parent_class;
} BoxrigGtk3TerminalAccessibleClass;

static void text_interface_init(AtkTextIface *interface);
G_DEFINE_TYPE_WITH_CODE(BoxrigGtk3TerminalAccessible,
    boxrig_gtk3_terminal_accessible, GTK_TYPE_CONTAINER_ACCESSIBLE,
    G_IMPLEMENT_INTERFACE(ATK_TYPE_TEXT, text_interface_init))

static const boxrig_ui_accessible_snapshot *snapshot(AtkText *text)
{
    BoxrigGtk3TerminalAccessible *accessible = (BoxrigGtk3TerminalAccessible *)text;
    return accessible->text.detached ? NULL : accessible->text.snapshot;
}

static gint character_count(AtkText *text)
{
    const boxrig_ui_accessible_snapshot *value = snapshot(text);
    return value == NULL ? 0 : (gint)value->character_count;
}

static gchar *get_text(AtkText *text, gint start, gint end)
{
    const boxrig_ui_accessible_snapshot *value = snapshot(text);
    gint count = character_count(text);
    if (end == -1 || end > count) end = count;
    if (start < 0 || start > end) return NULL;
    if (value == NULL) return g_strdup("");
    size_t first = value->characters[start].byte_offset;
    return g_strndup(value->text + first, value->characters[end].byte_offset - first);
}

static gunichar character_at(AtkText *text, gint offset)
{
    const boxrig_ui_accessible_snapshot *value = snapshot(text);
    return value == NULL || offset < 0 || (size_t)offset >= value->character_count
        ? 0 : value->characters[offset].scalar;
}

static gint caret_offset(AtkText *text)
{
    const boxrig_ui_accessible_snapshot *value = snapshot(text);
    return value == NULL ? 0 : (gint)value->caret;
}

static gchar *string_at(AtkText *text, gint offset, AtkTextGranularity granularity,
    gint *start, gint *end)
{
    boxrig_ui_accessible_range range;
    *start = *end = -1;
    if (offset < 0 || !boxrig_gtk_accessible_text_range(snapshot(text),
            (size_t)offset, (unsigned int)granularity, &range)) return NULL;
    *start = (gint)range.start;
    *end = (gint)range.end;
    return get_text(text, *start, *end);
}

static gint selection_count(AtkText *text)
{
    const boxrig_ui_accessible_snapshot *value = snapshot(text);
    return value == NULL ? 0 : (gint)value->selection_count;
}

static gchar *get_selection(AtkText *text, gint number, gint *start, gint *end)
{
    const boxrig_ui_accessible_snapshot *value = snapshot(text);
    *start = *end = -1;
    if (value == NULL || number < 0 || (size_t)number >= value->selection_count) return NULL;
    *start = (gint)value->selections[number].start;
    *end = (gint)value->selections[number].end;
    return get_text(text, *start, *end);
}

static AtkAttributeSet *attribute(AtkAttributeSet *set, const char *name, const char *value)
{
    AtkAttribute *entry = g_new(AtkAttribute, 1);
    entry->name = g_strdup(name);
    entry->value = g_strdup(value);
    return g_slist_prepend(set, entry);
}

static AtkAttributeSet *default_attributes(AtkText *text)
{
    (void)text;
    AtkAttributeSet *set = attribute(NULL, "editable", "false");
    return attribute(set, "wrap-mode", "char");
}

static AtkAttributeSet *run_attributes(AtkText *text, gint offset, gint *start, gint *end)
{
    const boxrig_ui_accessible_snapshot *value = snapshot(text);
    *start = *end = -1;
    if (value == NULL || offset < 0 || (size_t)offset >= value->character_count) return NULL;
    const boxrig_ui_accessible_character *character = &value->characters[offset];
    size_t first = (size_t)offset, last = first + 1;
    while (first > 0 && boxrig_gtk_accessible_same_attributes(character, &value->characters[first - 1])) first--;
    while (last < value->character_count && boxrig_gtk_accessible_same_attributes(character, &value->characters[last])) last++;
    *start = (gint)first;
    *end = (gint)last;
    AtkAttributeSet *set = default_attributes(text);
    set = attribute(set, "weight", character->attributes & BOXRIG_CELL_BOLD ? "700" : "400");
    set = attribute(set, "style", character->attributes & BOXRIG_CELL_ITALIC ? "italic" : "normal");
    set = attribute(set, "strikethrough", character->attributes & BOXRIG_CELL_STRIKETHROUGH ? "true" : "false");
    set = attribute(set, "invisible", character->attributes & BOXRIG_CELL_CONCEALED ? "true" : "false");
    set = attribute(set, "underline", character->underline == BOXRIG_UNDERLINE_NONE ? "none" :
        character->underline == BOXRIG_UNDERLINE_DOUBLE ? "double" : "single");
    char color[32];
    g_snprintf(color, sizeof(color), "%u,%u,%u", character->foreground.red * 257u,
        character->foreground.green * 257u, character->foreground.blue * 257u);
    set = attribute(set, "fg-color", color);
    g_snprintf(color, sizeof(color), "%u,%u,%u", character->background.red * 257u,
        character->background.green * 257u, character->background.blue * 257u);
    return attribute(set, "bg-color", color);
}

static bool origin(AtkText *text, AtkCoordType coords, gint *x, gint *y, gint *scale)
{
    GtkWidget *widget = gtk_accessible_get_widget(GTK_ACCESSIBLE(text));
    if (widget == NULL || !gtk_widget_get_realized(widget)) return false;
    GtkWidget *relative = coords == ATK_XY_PARENT ? gtk_widget_get_parent(widget) : gtk_widget_get_toplevel(widget);
    if (relative == NULL || !gtk_widget_translate_coordinates(widget, relative, 0, 0, x, y)) return false;
    if (coords == ATK_XY_SCREEN) {
        gint screen_x, screen_y;
        gdk_window_get_origin(gtk_widget_get_window(relative), &screen_x, &screen_y);
        *x += screen_x;
        *y += screen_y;
    } else if (coords != ATK_XY_WINDOW && coords != ATK_XY_PARENT) return false;
    *scale = gtk_widget_get_scale_factor(widget);
    return *scale > 0;
}

static void extents(AtkText *text, gint offset, gint *x, gint *y,
    gint *width, gint *height, AtkCoordType coords)
{
    if (x != NULL) *x = -1;
    if (y != NULL) *y = -1;
    if (width != NULL) *width = -1;
    if (height != NULL) *height = -1;
    const boxrig_ui_accessible_snapshot *value = snapshot(text);
    gint origin_x, origin_y, scale;
    if (value == NULL || offset < 0 || (size_t)offset >= value->character_count ||
        !origin(text, coords, &origin_x, &origin_y, &scale)) return;
    const boxrig_ui_accessible_character *character = &value->characters[offset];
    int64_t unit = (int64_t)BOXRIG_UI_FIXED_ONE * scale;
    int64_t left = character->x / unit + origin_x;
    int64_t top = character->y / unit + origin_y;
    int64_t w = (character->width + unit - 1) / unit;
    int64_t h = (character->height + unit - 1) / unit;
    if (left < INT_MIN || left > INT_MAX || top < INT_MIN || top > INT_MAX || w > INT_MAX || h > INT_MAX) return;
    if (x != NULL) *x = (gint)left;
    if (y != NULL) *y = (gint)top;
    if (width != NULL) *width = (gint)w;
    if (height != NULL) *height = (gint)h;
}

static gint offset_at_point(AtkText *text, gint x, gint y, AtkCoordType coords)
{
    const boxrig_ui_accessible_snapshot *value = snapshot(text);
    if (value == NULL) return -1;
    for (size_t i = 0; i < value->character_count; i++) {
        gint left, top, width, height;
        extents(text, (gint)i, &left, &top, &width, &height, coords);
        if (width > 0 && height > 0 && (int64_t)x >= left && (int64_t)y >= top &&
            (int64_t)x < (int64_t)left + width && (int64_t)y < (int64_t)top + height)
            return (gint)value->characters[i].grapheme_start;
    }
    return -1;
}

static void text_interface_init(AtkTextIface *interface)
{
    interface->get_text = get_text;
    interface->get_character_at_offset = character_at;
    interface->get_character_count = character_count;
    interface->get_caret_offset = caret_offset;
    interface->get_string_at_offset = string_at;
    interface->get_n_selections = selection_count;
    interface->get_selection = get_selection;
    interface->get_default_attributes = default_attributes;
    interface->get_run_attributes = run_attributes;
    interface->get_character_extents = extents;
    interface->get_offset_at_point = offset_at_point;
    /* Terminal cursor placement is owned by the child. Reading accessible
     * text must never synthesize input or pretend to edit its document. */
}

static AtkStateSet *ref_state_set(AtkObject *object)
{
    AtkObjectClass *parent = ATK_OBJECT_CLASS(boxrig_gtk3_terminal_accessible_parent_class);
    AtkStateSet *states = parent->ref_state_set(object);
    BoxrigGtk3TerminalAccessible *accessible = (BoxrigGtk3TerminalAccessible *)object;
    if (states != NULL && accessible->busy) atk_state_set_add_state(states, ATK_STATE_BUSY);
    return states;
}

static void finalize(GObject *object)
{
    BoxrigGtk3TerminalAccessible *accessible = (BoxrigGtk3TerminalAccessible *)object;
    boxrig_gtk_accessible_detach(&accessible->text);
    G_OBJECT_CLASS(boxrig_gtk3_terminal_accessible_parent_class)->finalize(object);
}

static void boxrig_gtk3_terminal_accessible_class_init(BoxrigGtk3TerminalAccessibleClass *klass)
{
    ATK_OBJECT_CLASS(klass)->ref_state_set = ref_state_set;
    G_OBJECT_CLASS(klass)->finalize = finalize;
}

static void boxrig_gtk3_terminal_accessible_init(BoxrigGtk3TerminalAccessible *accessible)
{
    (void)accessible;
}

void boxrig_gtk3_accessible_set_busy(BoxrigGtk3TerminalWidget *widget, bool busy)
{
    BoxrigGtk3TerminalAccessible *accessible = (BoxrigGtk3TerminalAccessible *)gtk_widget_get_accessible(GTK_WIDGET(widget));
    if (accessible->busy == busy) return;
    accessible->busy = busy;
    atk_object_notify_state_change(ATK_OBJECT(accessible), ATK_STATE_BUSY, busy);
}

static void notify(boxrig_gtk_accessible_event event,
    const boxrig_ui_accessible_change *change, void *context)
{
    AtkText *text = context;
    if (event == BOXRIG_GTK_ACCESSIBLE_REMOVE || event == BOXRIG_GTK_ACCESSIBLE_INSERT) {
        size_t count = event == BOXRIG_GTK_ACCESSIBLE_REMOVE ? change->removed : change->inserted;
        gchar *slice = get_text(text, (gint)change->start, (gint)(change->start + count));
        g_signal_emit_by_name(text, event == BOXRIG_GTK_ACCESSIBLE_REMOVE ?
            "text-remove::system" : "text-insert::system", (gint)change->start, (gint)count, slice);
        g_free(slice);
    } else if (event == BOXRIG_GTK_ACCESSIBLE_CARET) {
        g_signal_emit_by_name(text, "text-caret-moved", caret_offset(text));
    } else {
        g_signal_emit_by_name(text, event == BOXRIG_GTK_ACCESSIBLE_SELECTION ?
            "text-selection-changed" : "text-attributes-changed");
    }
}

boxrig_ui_status boxrig_gtk3_accessible_update(BoxrigGtk3TerminalWidget *widget)
{
    BoxrigGtk3TerminalAccessible *accessible = (BoxrigGtk3TerminalAccessible *)gtk_widget_get_accessible(GTK_WIDGET(widget));
    g_object_ref(accessible);
    boxrig_ui_status status = boxrig_gtk_accessible_publish(&accessible->text,
        boxrig_gtk3_terminal_widget_view(widget), notify, accessible);
    g_object_unref(accessible);
    return status;
}

void boxrig_gtk3_accessible_detach(BoxrigGtk3TerminalWidget *widget)
{
    BoxrigGtk3TerminalAccessible *accessible = (BoxrigGtk3TerminalAccessible *)gtk_widget_get_accessible(GTK_WIDGET(widget));
    boxrig_gtk_accessible_detach(&accessible->text);
}
