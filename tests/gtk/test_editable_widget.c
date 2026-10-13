/* boxrig - Under MIT. */
#include "../editable_fixture.h"
#include <gtk/gtk.h>
#if GTK_MAJOR_VERSION == 3
#include "boxrig/gtk3.h"
typedef BoxrigGtk3TerminalWidget terminal_widget;
typedef boxrig_gtk3_terminal_widget_config widget_config;
#define API(name) boxrig_gtk3_terminal_widget_##name
#else
#include "boxrig/gtk4.h"
typedef BoxrigGtk4TerminalWidget terminal_widget;
typedef boxrig_gtk4_terminal_widget_config widget_config;
#define API(name) boxrig_gtk4_terminal_widget_##name
#endif
#include "boxrig/ui/editable.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL GTK %d %s:%d: %s\n", GTK_MAJOR_VERSION, __FILE__, __LINE__, #condition); \
    abort(); } } while (0)

static GtkIMContext *native_im;
static char surrounding[1024];
static int surrounding_cursor, surrounding_anchor;
static unsigned int surrounding_calls;
static bool fail_edit_allocation;

boxrig_ui_status __real_boxrig_ui_editable_document_apply_edit(
    boxrig_ui_editable_document *, const boxrig_ui_editable_edit *);
/** Injects a preparation failure at the document boundary, before acceptance. */
boxrig_ui_status __wrap_boxrig_ui_editable_document_apply_edit(
    boxrig_ui_editable_document *document, const boxrig_ui_editable_edit *edit)
{
    if (fail_edit_allocation) {
        fail_edit_allocation = false;
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    return __real_boxrig_ui_editable_document_apply_edit(document, edit);
}

GtkIMContext *__real_gtk_im_multicontext_new(void);
/** Observes the real widget's context without replacing the native object. */
GtkIMContext *__wrap_gtk_im_multicontext_new(void)
{
    native_im = __real_gtk_im_multicontext_new();
    return native_im;
}

#if GTK_MAJOR_VERSION == 3
void __real_gtk_im_context_set_surrounding(GtkIMContext *, const char *, int, int);
void __wrap_gtk_im_context_set_surrounding(GtkIMContext *context, const char *text, int length, int cursor)
{
    CHECK(length >= 0 && (size_t)length < sizeof(surrounding));
    memcpy(surrounding, text, (size_t)length);
    surrounding[length] = '\0';
    surrounding_cursor = surrounding_anchor = cursor;
    surrounding_calls++;
    __real_gtk_im_context_set_surrounding(context, text, length, cursor);
}
#else
static GtkEventController *native_focus;
GtkEventController *__real_gtk_event_controller_focus_new(void);
GtkEventController *__wrap_gtk_event_controller_focus_new(void)
{
    native_focus = __real_gtk_event_controller_focus_new();
    return native_focus;
}
void __real_gtk_im_context_set_surrounding_with_selection(GtkIMContext *, const char *, int, int, int);
void __wrap_gtk_im_context_set_surrounding_with_selection(GtkIMContext *context,
    const char *text, int length, int cursor, int anchor)
{
    CHECK(length >= 0 && (size_t)length < sizeof(surrounding));
    memcpy(surrounding, text, (size_t)length);
    surrounding[length] = '\0';
    surrounding_cursor = cursor;
    surrounding_anchor = anchor;
    surrounding_calls++;
    __real_gtk_im_context_set_surrounding_with_selection(context, text, length, cursor, anchor);
}
#endif

typedef struct owner {
    char text[1024];
    uint64_t identity, revision;
    unsigned int calls, accepted;
    boxrig_ui_status disposition;
} owner;

static boxrig_ui_status apply(const boxrig_ui_editable_replacement_request *request,
    const boxrig_ui_editable_document_snapshot *proposed, void *data)
{
    owner *document = data;
    document->calls++;
    if (document->disposition != BOXRIG_UI_OK) return document->disposition;
    CHECK(request->document_identity == document->identity);
    CHECK(request->base_document_revision == document->revision);
    size_t start = (size_t)(request->range.offset - 100);
    size_t length = strlen(document->text);
    CHECK(start + request->range.length <= length);
    CHECK(length - request->range.length + request->replacement_length < sizeof(document->text));
    memmove(document->text + start + request->replacement_length,
        document->text + start + request->range.length, length - start - request->range.length + 1);
    if (request->replacement_length != 0)
        memcpy(document->text + start, request->replacement, request->replacement_length);
    CHECK(strlen(document->text) == proposed->context_length);
    CHECK(memcmp(document->text, proposed->context, proposed->context_length) == 0);
    document->revision = proposed->document_revision;
    document->accepted++;
    return BOXRIG_UI_OK;
}

static void publish(boxrig_ui_editable_document *endpoint, owner *document,
    const char *text, size_t cursor, size_t anchor)
{
    strcpy(document->text, text);
    document->revision++;
    boxrig_ui_editable_context_input context = {document->identity, document->revision,
        text, strlen(text), 100, 100 + cursor, 100 + anchor};
    CHECK(boxrig_ui_editable_document_publish_context(endpoint, &context) == BOXRIG_UI_OK);
}

static void frame(terminal_widget *widget)
{
    boxrig_ui_component_frame_result result;
    CHECK(API(frame)(widget, &result) == BOXRIG_UI_OK);
    boxrig_ui_component_diagnostic diagnostic;
    CHECK(API(get_diagnostic)(widget, &diagnostic) == BOXRIG_UI_OK);
    CHECK(!diagnostic.failed);
}

static void focus(terminal_widget *widget, bool focused)
{
#if GTK_MAJOR_VERSION == 3
    GdkEventFocus event = {0};
    gboolean handled = FALSE;
    event.type = GDK_FOCUS_CHANGE;
    event.window = gtk_widget_get_window(GTK_WIDGET(widget));
    event.in = focused;
    g_signal_emit_by_name(widget, focused ? "focus-in-event" : "focus-out-event", &event, &handled);
#else
    (void)widget;
    g_signal_emit_by_name(native_focus, focused ? "enter" : "leave");
#endif
}

static void retrieve(const char *text, int cursor, int anchor)
{
    gboolean handled = FALSE;
    unsigned int before = surrounding_calls;
    g_signal_emit_by_name(native_im, "retrieve-surrounding", &handled);
    CHECK(handled && surrounding_calls > before);
    CHECK(strcmp(surrounding, text) == 0 && surrounding_cursor == cursor);
#if GTK_MAJOR_VERSION == 3
    CHECK(surrounding_anchor == cursor);
    (void)anchor;
#else
    CHECK(surrounding_anchor == anchor);
#endif
}

static bool remove_characters(int offset, int count)
{
    gboolean handled = FALSE;
    g_signal_emit_by_name(native_im, "delete-surrounding", offset, count, &handled);
    return handled;
}

static void test_shared_document_states(terminal_widget *widget, boxrig_terminal *terminal)
{
    boxrig_ui_editable_document *endpoint = boxrig_ui_view_editable_document(API(view)(widget));
    owner document = {.identity = 8, .disposition = BOXRIG_UI_OK};
    for (size_t index = 0; index < sizeof(boxrig_editable_cases) / sizeof(boxrig_editable_cases[0]); index++) {
        CHECK(boxrig_ui_editable_document_clear(endpoint) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_editable_document_set_apply_handler(endpoint,
            boxrig_editable_cases[index].synchronous_edit_available ? apply : NULL,
            boxrig_editable_cases[index].synchronous_edit_available ? &document : NULL) == BOXRIG_UI_OK);
        strcpy(document.text, boxrig_editable_fixture.text);
        if (boxrig_editable_cases[index].context_available)
            publish(endpoint, &document, boxrig_editable_fixture.text,
                boxrig_editable_fixture.cursor, boxrig_editable_fixture.anchor);
        frame(widget);
        boxrig_ui_editable_capabilities caps = boxrig_ui_editable_document_capabilities(endpoint);
        CHECK(caps.context_available == boxrig_editable_cases[index].context_available);
        CHECK(caps.synchronous_edit_available == boxrig_editable_cases[index].synchronous_edit_available);
        CHECK(!caps.reconversion_available && !caps.busy);
        gboolean handled = FALSE;
        g_signal_emit_by_name(native_im, "retrieve-surrounding", &handled);
        CHECK(handled == boxrig_editable_cases[index].context_available);
        if (handled) retrieve(boxrig_editable_fixture.text, 9, 9);
        if (handled) CHECK(remove_characters(-1, 1) == boxrig_editable_cases[index].synchronous_edit_available);
        g_signal_emit_by_name(native_im, "commit", boxrig_editable_fixture.commit);
        frame(widget);
        CHECK(strcmp(document.text, boxrig_editable_cases[index].after_synchronous_edit) == 0);
        const uint8_t *bytes;
        size_t length = boxrig_terminal_output_peek(terminal, &bytes);
        CHECK(length == boxrig_editable_cases[index].terminal_bytes);
        if (length != 0) {
            CHECK(memcmp(bytes, boxrig_editable_fixture.commit, length) == 0);
            CHECK(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
        }
        printf("PASS GTK %d shared editable state=%s\n", GTK_MAJOR_VERSION, boxrig_editable_cases[index].name);
    }
    /* Native IM contexts use strings; an embedded NUL cannot be reported as
     * a successfully supplied context or followed by a partial edit. */
    const char binary[] = {'A', '\0', 'B'};
    boxrig_ui_editable_context_input input = {document.identity, ++document.revision,
        binary, sizeof(binary), 100, 103, 103};
    CHECK(boxrig_ui_editable_document_publish_context(endpoint, &input) == BOXRIG_UI_OK);
    frame(widget);
    unsigned int before = surrounding_calls, owner_calls = document.calls;
    gboolean handled = TRUE;
    g_signal_emit_by_name(native_im, "retrieve-surrounding", &handled);
    CHECK(!handled && surrounding_calls == before);
    CHECK(!remove_characters(-1, 1));
    g_signal_emit_by_name(native_im, "commit", "discard");
    frame(widget);
    boxrig_ui_editable_document_snapshot snapshot;
    CHECK(boxrig_ui_editable_document_get_snapshot(endpoint, &snapshot));
    CHECK(snapshot.context_length == sizeof(binary) && memcmp(snapshot.context, binary, sizeof(binary)) == 0);
    CHECK(document.calls == owner_calls);
    const uint8_t *output;
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_ui_editable_document_clear(endpoint) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_editable_document_set_apply_handler(endpoint, NULL, NULL) == BOXRIG_UI_OK);
    frame(widget);
}

int main(int argc, char **argv)
{
#if GTK_MAJOR_VERSION == 3
    gtk_init(NULL, NULL);
#else
    gtk_init();
#endif
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    terminal_widget *widget = NULL;
    widget_config wc = API(config_default)();
    wc.render_policy = argc > 1 && strcmp(argv[1], "opengl") == 0
        ? BOXRIG_GTK_RENDER_OPENGL_REQUIRED : BOXRIG_GTK_RENDER_CAIRO;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(API(create)(model, &wc, &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
#if GTK_MAJOR_VERSION == 3
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(widget));
    gtk_widget_show_all(window);
    GtkAllocation allocation = {0, 0, 320, 160};
    gtk_widget_size_allocate(GTK_WIDGET(widget), &allocation);
#else
    GtkWidget *window = gtk_window_new();
    gtk_window_set_child(GTK_WINDOW(window), GTK_WIDGET(widget));
    gtk_window_present(GTK_WINDOW(window));
    gtk_widget_allocate(GTK_WIDGET(widget), 320, 160, -1, NULL);
#endif
    focus(widget, true);
    frame(widget);
    boxrig_ui_editable_document *endpoint = boxrig_ui_view_editable_document(API(view)(widget));
    test_shared_document_states(widget, terminal);
    owner document = {.identity = 7, .disposition = BOXRIG_UI_OK};
    gboolean handled = TRUE;
    g_signal_emit_by_name(native_im, "retrieve-surrounding", &handled);
    CHECK(!handled);
    g_signal_emit_by_name(native_im, "commit", "日本");
    frame(widget);
    const uint8_t *bytes;
    CHECK(boxrig_terminal_output_peek(terminal, &bytes) == 6 && memcmp(bytes, "日本", 6) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 6) == BOXRIG_STATUS_OK);

    CHECK(boxrig_ui_editable_document_set_apply_handler(endpoint, apply, &document) == BOXRIG_UI_OK);
    publish(endpoint, &document, "前é🙂\n後", 10, 10);
    frame(widget);
    retrieve(document.text, 10, 10);
    CHECK(remove_characters(-2, 1));
    CHECK(strcmp(document.text, "前e🙂\n後") == 0 && document.accepted == 1);
    retrieve(document.text, 8, 8);
    g_signal_emit_by_name(native_im, "commit", "語");
    frame(widget);
    CHECK(strcmp(document.text, "前e🙂語\n後") == 0 && document.accepted == 2);

    g_signal_emit_by_name(native_im, "commit", "A");
    g_signal_emit_by_name(native_im, "commit", "B");
    CHECK(remove_characters(-1, 1));
    CHECK(strcmp(document.text, "前e🙂語A\n後") == 0 && document.accepted == 5);

    GtkIMContext *old = g_object_ref(native_im);
    document.disposition = BOXRIG_UI_UNAVAILABLE;
    CHECK(!remove_characters(-1, 1));
    g_signal_emit_by_name(old, "commit", "拒否後");
    frame(widget);
    g_signal_emit_by_name(old, "commit", "遅延");
    document.disposition = BOXRIG_UI_OK;
    frame(widget);
    CHECK(strcmp(document.text, "前e🙂語A\n後") == 0 && document.accepted == 5);
    g_object_unref(old);
    g_signal_emit_by_name(native_im, "commit", "次");
    frame(widget);
    CHECK(strcmp(document.text, "前e🙂語A次\n後") == 0 && document.accepted == 6);

    old = g_object_ref(native_im);
    g_signal_emit_by_name(old, "commit", "古い");
    document.identity++;
    publish(endpoint, &document, "新文書", 3, 9);
    frame(widget);
    g_signal_emit_by_name(old, "commit", "別文書");
    g_object_unref(old);
    retrieve("新文書", 3, 9);
    g_signal_emit_by_name(native_im, "commit", "規");
    frame(widget);
    CHECK(strcmp(document.text, "新規") == 0);

    document.disposition = BOXRIG_UI_BUSY;
    unsigned int accepted = document.accepted;
    g_signal_emit_by_name(native_im, "commit", "待機");
    frame(widget);
    CHECK(document.accepted == accepted && strcmp(document.text, "新規") == 0);
    document.disposition = BOXRIG_UI_OK;
    frame(widget);
    frame(widget);
    CHECK(document.accepted == accepted + 1 && strcmp(document.text, "新規待機") == 0);

    document.disposition = BOXRIG_UI_BUSY;
    g_signal_emit_by_name(native_im, "commit", "中止");
    frame(widget);
    CHECK(!remove_characters(-1, 1));
    document.disposition = BOXRIG_UI_OK;
    frame(widget);
    CHECK(strcmp(document.text, "新規待機") == 0);

    old = g_object_ref(native_im);
    g_signal_emit_by_name(old, "commit", "focus前");
    focus(widget, false);
    g_signal_emit_by_name(old, "commit", "focus後");
    frame(widget);
    focus(widget, true);
    frame(widget);
    g_signal_emit_by_name(old, "commit", "focus復帰後");
    g_object_unref(old);
    frame(widget);
    CHECK(strcmp(document.text, "新規待機") == 0);
    CHECK(!remove_characters(INT_MIN, INT_MAX));
    frame(widget);
    CHECK(!remove_characters(0, -1));
    frame(widget);



    accepted = document.accepted;
    fail_edit_allocation = true;
    CHECK(!remove_characters(-1, 1));
    CHECK(!fail_edit_allocation);
    g_signal_emit_by_name(native_im, "commit", "確保失敗後");
    frame(widget);
    CHECK(document.accepted == accepted && strcmp(document.text, "新規待機") == 0);
    fail_edit_allocation = true;
    g_signal_emit_by_name(native_im, "commit", "確保失敗");
    frame(widget);
    CHECK(!fail_edit_allocation);
    frame(widget);
    CHECK(document.accepted == accepted && strcmp(document.text, "新規待機") == 0);

    /* Cursor-only publication invalidates an already pending native commit. */
    document.disposition = BOXRIG_UI_BUSY;
    g_signal_emit_by_name(native_im, "commit", "古い位置");
    frame(widget);
    boxrig_ui_editable_context_input moved = {document.identity, document.revision,
        document.text, strlen(document.text), 100, 100, 100};
    CHECK(boxrig_ui_editable_document_publish_context(endpoint, &moved) == BOXRIG_UI_OK);
    g_signal_emit_by_name(native_im, "commit", "移動後の遅延");
    document.disposition = BOXRIG_UI_OK;
    frame(widget);
    retrieve(document.text, 0, 0);
    CHECK(document.accepted == accepted && strcmp(document.text, "新規待機") == 0);

    old = g_object_ref(native_im);
    CHECK(boxrig_ui_editable_document_invalidate_input(endpoint) == BOXRIG_UI_OK);
    g_signal_emit_by_name(old, "commit", "取消後");
    frame(widget);
    g_signal_emit_by_name(old, "commit", "取消後の遅延");
    g_object_unref(old);
    frame(widget);
    CHECK(document.accepted == accepted);

    publish(endpoint, &document, "", 0, 0);
    frame(widget);
    retrieve("", 0, 0);
    g_signal_emit_by_name(native_im, "commit", "空");
    frame(widget);
    CHECK(strcmp(document.text, "空") == 0);
    CHECK(boxrig_ui_editable_document_set_apply_handler(endpoint, NULL, NULL) == BOXRIG_UI_OK);
    frame(widget);
    retrieve("空", 3, 3);
    CHECK(!remove_characters(-1, 1));
    g_signal_emit_by_name(native_im, "commit", "不可");
    frame(widget);
    CHECK(strcmp(document.text, "空") == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &bytes) == 0);

    CHECK(boxrig_ui_editable_document_clear(endpoint) == BOXRIG_UI_OK);
    frame(widget);
    g_signal_emit_by_name(native_im, "commit", "端末");
    frame(widget);
    CHECK(boxrig_terminal_output_peek(terminal, &bytes) == 6 && memcmp(bytes, "端末", 6) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 6) == BOXRIG_STATUS_OK);
#if GTK_MAJOR_VERSION == 3
    /* A synchronous deletion cannot overtake queued geometry-dependent input. */
    CHECK(boxrig_ui_editable_document_set_apply_handler(endpoint, apply, &document) == BOXRIG_UI_OK);
    publish(endpoint, &document, "終了前", 9, 9);
    frame(widget);
    GdkEventButton button = {0};
    button.type = GDK_BUTTON_PRESS;
    button.window = gtk_widget_get_window(GTK_WIDGET(widget));
    button.button = 1;
    button.x = button.y = 10;
    handled = FALSE;
    g_signal_emit_by_name(widget, "button-press-event", &button, &handled);
    CHECK(handled);
    CHECK(!remove_characters(-1, 1));
    frame(widget);
    button.type = GDK_BUTTON_RELEASE;
    g_signal_emit_by_name(widget, "button-release-event", &button, &handled);
    frame(widget);
    CHECK(strcmp(document.text, "終了前") == 0);
#endif
    old = g_object_ref(native_im);
    CHECK(API(quiesce)(widget) == BOXRIG_UI_OK);
    g_signal_emit_by_name(old, "commit", "終了後");
#if GTK_MAJOR_VERSION == 3
    gtk_widget_destroy(window);
#else
    gtk_window_destroy(GTK_WINDOW(window));
#endif
    g_object_unref(widget);
    g_signal_emit_by_name(old, "commit", "破棄後");
    g_object_unref(old);
    CHECK(boxrig_terminal_output_peek(terminal, &bytes) == 0);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    printf("PASS GTK %d editable context, deletion, FIFO commit, rejection, focus and late events\n", GTK_MAJOR_VERSION);
    return 0;
}
