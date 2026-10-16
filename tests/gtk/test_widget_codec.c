/* boxrig - Under MIT. */
#ifdef BOXRIG_TEST_GTK3
#include "boxrig/gtk3/terminal_widget.h"
#define Widget BoxrigGtk3TerminalWidget
#define widget_config boxrig_gtk3_terminal_widget_config
#define config_default boxrig_gtk3_terminal_widget_config_default
#define widget_create boxrig_gtk3_terminal_widget_create
#define widget_quiesce boxrig_gtk3_terminal_widget_quiesce
#define widget_queue_paste boxrig_gtk3_terminal_widget_queue_paste
#define widget_frame boxrig_gtk3_terminal_widget_frame
#define widget_diagnostic boxrig_gtk3_terminal_widget_get_diagnostic
#define widget_set_input_config boxrig_gtk3_terminal_widget_set_input_config
#define widget_set_input_enabled boxrig_gtk3_terminal_widget_set_input_enabled
#define widget_get_input_state boxrig_gtk3_terminal_widget_get_input_state
#define widget_composition boxrig_gtk3_terminal_widget_composition
#else
#include "boxrig/gtk4/terminal_widget.h"
#define Widget BoxrigGtk4TerminalWidget
#define widget_config boxrig_gtk4_terminal_widget_config
#define config_default boxrig_gtk4_terminal_widget_config_default
#define widget_create boxrig_gtk4_terminal_widget_create
#define widget_quiesce boxrig_gtk4_terminal_widget_quiesce
#define widget_queue_paste boxrig_gtk4_terminal_widget_queue_paste
#define widget_frame boxrig_gtk4_terminal_widget_frame
#define widget_diagnostic boxrig_gtk4_terminal_widget_get_diagnostic
#define widget_set_input_config boxrig_gtk4_terminal_widget_set_input_config
#define widget_set_input_enabled boxrig_gtk4_terminal_widget_set_input_enabled
#define widget_get_input_state boxrig_gtk4_terminal_widget_get_input_state
#define widget_composition boxrig_gtk4_terminal_widget_composition
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Capture the native input objects while keeping their normal ownership. */
static GtkIMContext *native_im;
#ifdef BOXRIG_TEST_GTK3
static Widget *filter_owner;
static bool commit_on_release;
gboolean __real_gtk_im_context_filter_keypress(GtkIMContext *context, GdkEventKey *event);
gboolean __wrap_gtk_im_context_filter_keypress(GtkIMContext *context, GdkEventKey *event)
{
    if (filter_owner != NULL)
        assert(widget_set_input_enabled(filter_owner, false) == BOXRIG_UI_BUSY);
    if (commit_on_release && event->type == GDK_KEY_RELEASE) {
        g_signal_emit_by_name(context, "commit", "leak");
        return TRUE;
    }
    return __real_gtk_im_context_filter_keypress(context, event);
}
#endif
GtkIMContext *__real_gtk_im_multicontext_new(void);
GtkIMContext *__wrap_gtk_im_multicontext_new(void)
{
    native_im = __real_gtk_im_multicontext_new();
    return native_im;
}
#ifndef BOXRIG_TEST_GTK3
static GtkEventController *native_key;
static GtkEventController *native_focus;
GtkEventController *__real_gtk_event_controller_key_new(void);
GtkEventController *__wrap_gtk_event_controller_key_new(void)
{
    native_key = __real_gtk_event_controller_key_new();
    return native_key;
}
GtkEventController *__real_gtk_event_controller_focus_new(void);
GtkEventController *__wrap_gtk_event_controller_focus_new(void)
{
    native_focus = __real_gtk_event_controller_focus_new();
    return native_focus;
}
#endif

int main(void)
{
#ifdef BOXRIG_TEST_GTK3
    gtk_init(NULL, NULL);
#else
    gtk_init();
#endif
    for (unsigned encoding = 0; encoding < 2; encoding++) {
        const uint8_t mapped[] = {0x1b, 'U', 0};
        const boxrig_key_mapping mapping = {BOXRIG_KEY_UP, 0, mapped, sizeof(mapped)};
        boxrig_terminal_config tc = boxrig_terminal_config_default();
        tc.encoding = (boxrig_encoding)encoding;
        tc.input.enter_newline = BOXRIG_NEWLINE_LF;
        tc.paste_newline = BOXRIG_NEWLINE_CRLF;
        tc.input.key_mappings = &mapping;
        tc.input.key_mapping_count = 1;
        tc.output_capacity = 6;
        widget_config config = config_default();
        /* This fixture injects GTK commit signals; discovery may instead
         * select a direct daemon and replace that GTK input context. */
        config.ime_policy = BOXRIG_GTK_IME_GTK;
        config.paste_chunk_size = 1;
        boxrig_terminal *terminal = NULL;
        boxrig_ui_model *model = NULL;
        Widget *widget = NULL;
        assert(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
        assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
        assert(widget_create(model, &config, &widget) == BOXRIG_UI_OK);
        g_object_ref_sink(widget);
#ifdef BOXRIG_TEST_GTK3
        GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(widget));
        gtk_widget_show_all(window);
        GtkAllocation allocation = {0, 0, 240, 80};
        gtk_widget_size_allocate(GTK_WIDGET(widget), &allocation);
#else
        gtk_widget_allocate(GTK_WIDGET(widget), 240, 80, -1, NULL);
#endif
        size_t consumed = 0;
        assert(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[?2004h", 8, &consumed) == BOXRIG_STATUS_OK);
        const uint8_t text[] = "A日本\r\n🙂B";
        assert(widget_queue_paste(widget, text, sizeof(text) - 1) == BOXRIG_UI_OK);
        assert(widget_set_input_config(widget, &tc.input) == BOXRIG_UI_BUSY);
        const uint8_t *output;
        uint8_t observed[64];
        size_t observed_length = 0;
        bool complete = false;
        for (unsigned attempt = 0; attempt < 32; attempt++) {
            size_t length = boxrig_terminal_output_peek(terminal, &output);
            assert(length <= sizeof(observed) - observed_length);
            if (length != 0) {
                memcpy(observed + observed_length, output, length);
                observed_length += length;
                assert(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
            }
            boxrig_ui_component_frame_result frame;
            assert(widget_frame(widget, &frame) == BOXRIG_UI_OK);
            boxrig_ui_component_diagnostic diagnostic;
            assert(widget_diagnostic(widget, &diagnostic) == BOXRIG_UI_OK);
            assert(!diagnostic.failed);
            if (diagnostic.native_input_queue_depth == 0 &&
                boxrig_terminal_output_peek(terminal, &output) == 0) {
                complete = true; break;
            }
        }
        assert(complete);
        const char *expected = encoding ? "\033[200~A\x93\xfa\x96\x7b\r\n?B\033[201~" :
            "\033[200~A日本\r\n🙂B\033[201~";
        assert(observed_length == strlen(expected) && memcmp(observed, expected, observed_length) == 0);

        gboolean handled = FALSE;
#ifdef BOXRIG_TEST_GTK3
        GdkEventFocus focus = {0};
        focus.type = GDK_FOCUS_CHANGE;
        focus.window = gtk_widget_get_window(GTK_WIDGET(widget));
        focus.in = TRUE;
        g_signal_emit_by_name(widget, "focus-in-event", &focus, &handled);
#else
        g_signal_emit_by_name(native_focus, "enter");
#endif
        g_signal_emit_by_name(native_im, "commit", "日本");
        boxrig_ui_component_frame_result frame;
        assert(widget_frame(widget, &frame) == BOXRIG_UI_OK);
        const char *committed = encoding ? "\x93\xfa\x96\x7b" : "日本";
        size_t length = boxrig_terminal_output_peek(terminal, &output);
        assert(length == strlen(committed) && memcmp(output, committed, length) == 0);
        assert(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
        const guint keys[] = {GDK_KEY_Up, GDK_KEY_Return};
        const guint codes[] = {111, 36};
        boxrig_input_config replacement = boxrig_input_config_default();
        replacement.enter_newline = BOXRIG_NEWLINE_CR;
        for (unsigned index = 0; index < 2; index++) {
            handled = FALSE;
#ifdef BOXRIG_TEST_GTK3
            GdkEventKey event = {0};
            event.type = GDK_KEY_PRESS;
            event.window = gtk_widget_get_window(GTK_WIDGET(widget));
            event.keyval = keys[index];
            event.hardware_keycode = codes[index];
            g_signal_emit_by_name(widget, "key-press-event", &event, &handled);
            assert(handled);
            assert(widget_set_input_config(widget, &replacement) == BOXRIG_UI_BUSY);
            event.type = GDK_KEY_RELEASE;
            g_signal_emit_by_name(widget, "key-release-event", &event, &handled);
#else
            g_signal_emit_by_name(native_key, "key-pressed", keys[index], codes[index], 0, &handled);
            assert(handled);
            assert(widget_set_input_config(widget, &replacement) == BOXRIG_UI_BUSY);
            g_signal_emit_by_name(native_key, "key-released", keys[index], codes[index], 0);
#endif
        }
        assert(widget_set_input_config(widget, &replacement) == BOXRIG_UI_BUSY);
        assert(widget_frame(widget, &frame) == BOXRIG_UI_OK);
        const uint8_t key_expected[] = {0x1b, 'U', 0, '\n'};
        assert(boxrig_terminal_output_peek(terminal, &output) == sizeof(key_expected));
        assert(memcmp(output, key_expected, sizeof(key_expected)) == 0);
        assert(boxrig_terminal_output_consume(terminal, sizeof(key_expected)) == BOXRIG_STATUS_OK);
        assert(widget_set_input_config(widget, &replacement) == BOXRIG_UI_OK);
        boxrig_input_config accepted;
        assert(boxrig_terminal_get_input_config(terminal, &accepted) == BOXRIG_STATUS_OK);
        assert(accepted.enter_newline == BOXRIG_NEWLINE_CR && accepted.key_mapping_count == 0);
#ifdef BOXRIG_TEST_GTK3
        GdkEventKey changed_key = {0};
        changed_key.type = GDK_KEY_PRESS;
        changed_key.window = gtk_widget_get_window(GTK_WIDGET(widget));
        changed_key.keyval = GDK_KEY_Return;
        changed_key.hardware_keycode = 36;
        g_signal_emit_by_name(widget, "key-press-event", &changed_key, &handled);
        changed_key.type = GDK_KEY_RELEASE;
        g_signal_emit_by_name(widget, "key-release-event", &changed_key, &handled);
#else
        g_signal_emit_by_name(native_key, "key-pressed", GDK_KEY_Return, 36, 0, &handled);
        g_signal_emit_by_name(native_key, "key-released", GDK_KEY_Return, 36, 0);
#endif
        assert(widget_frame(widget, &frame) == BOXRIG_UI_OK);
        assert(boxrig_terminal_output_peek(terminal, &output) == 1 && output[0] == '\r');
        assert(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
        boxrig_composition_event preedit = {0};
        preedit.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
        preedit.text = "未確定";
        preedit.text_length = strlen(preedit.text);
        assert(boxrig_composition_apply_event(widget_composition(widget), &preedit) == BOXRIG_COMPOSITION_OK);
        assert(widget_set_input_config(widget, &tc.input) == BOXRIG_UI_BUSY);
        preedit.type = BOXRIG_COMPOSITION_CANCEL;
        preedit.text = NULL;
        preedit.text_length = 0;
        assert(boxrig_composition_apply_event(widget_composition(widget), &preedit) == BOXRIG_COMPOSITION_OK);
        assert(widget_set_input_config(widget, &tc.input) == BOXRIG_UI_OK);
        boxrig_ui_input_state input_state;
        assert(widget_get_input_state(widget, &input_state) == BOXRIG_UI_OK);
        assert(input_state.phase == BOXRIG_UI_INPUT_ENABLED);
        uint64_t old_generation = input_state.generation;
        GtkIMContext *old_im = g_object_ref(native_im);
        assert(boxrig_terminal_send_text(terminal, "filled", 6) == BOXRIG_STATUS_OK);
        assert(widget_queue_paste(widget, (const uint8_t *)"discard", 7) == BOXRIG_UI_OK);
        g_signal_emit_by_name(old_im, "commit", "old");
        assert(widget_set_input_enabled(widget, false) == BOXRIG_UI_OK);
        assert(widget_get_input_state(widget, &input_state) == BOXRIG_UI_OK);
        assert(input_state.phase == BOXRIG_UI_INPUT_DISABLED && input_state.generation > old_generation);
        assert(boxrig_terminal_output_peek(terminal, &output) == 6);
        assert(memcmp(output, "filled", 6) == 0);
        assert(boxrig_terminal_output_consume(terminal, 6) == BOXRIG_STATUS_OK);
        assert(widget_queue_paste(widget, (const uint8_t *)"blocked", 7) == BOXRIG_UI_UNAVAILABLE);
        g_signal_emit_by_name(old_im, "commit", "late");
        assert(widget_frame(widget, &frame) == BOXRIG_UI_OK);
        assert(boxrig_terminal_output_peek(terminal, &output) == 0);
        assert(widget_set_input_enabled(widget, true) == BOXRIG_UI_OK);
        assert(native_im != old_im);
        g_signal_emit_by_name(old_im, "commit", "late after enable");
        assert(widget_frame(widget, &frame) == BOXRIG_UI_OK);
        assert(boxrig_terminal_output_peek(terminal, &output) == 0);
        g_object_unref(old_im);
        g_signal_emit_by_name(native_im, "commit", "B");
        assert(widget_frame(widget, &frame) == BOXRIG_UI_OK);
        assert(boxrig_terminal_output_peek(terminal, &output) == 1 && output[0] == 'B');
        assert(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);

        /* A started bracketed paste closes after output capacity becomes available. */
        assert(widget_queue_paste(widget, (const uint8_t *)"discard", 7) == BOXRIG_UI_OK);
        assert(widget_frame(widget, &frame) == BOXRIG_UI_OK);
        assert(boxrig_terminal_output_peek(terminal, &output) == 6);
        assert(memcmp(output, "\x1b[200~", 6) == 0);
        assert(widget_set_input_enabled(widget, false) == BOXRIG_UI_BUSY);
        assert(widget_get_input_state(widget, &input_state) == BOXRIG_UI_OK);
        assert(input_state.phase == BOXRIG_UI_INPUT_STOPPING);
        assert(widget_set_input_enabled(widget, true) == BOXRIG_UI_BUSY);
        assert(widget_set_input_config(widget, &tc.input) == BOXRIG_UI_BUSY);
        assert(boxrig_terminal_output_consume(terminal, 6) == BOXRIG_STATUS_OK);
        assert(boxrig_terminal_feed(terminal, (const uint8_t *)"\x1b[5n", 4, &consumed) == BOXRIG_STATUS_OK);
        assert(widget_frame(widget, &frame) == BOXRIG_UI_OK);
        assert(widget_get_input_state(widget, &input_state) == BOXRIG_UI_OK);
        assert(input_state.phase == BOXRIG_UI_INPUT_STOPPING);
        assert(boxrig_terminal_output_peek(terminal, &output) == 4);
        assert(memcmp(output, "\x1b[0n", 4) == 0);
        assert(boxrig_terminal_output_consume(terminal, 4) == BOXRIG_STATUS_OK);
        assert(widget_frame(widget, &frame) == BOXRIG_UI_OK);
        assert(widget_get_input_state(widget, &input_state) == BOXRIG_UI_OK);
        assert(input_state.phase == BOXRIG_UI_INPUT_DISABLED);
        assert(boxrig_terminal_output_peek(terminal, &output) == 6);
        assert(memcmp(output, "\x1b[201~", 6) == 0);
        assert(boxrig_terminal_output_consume(terminal, 6) == BOXRIG_STATUS_OK);
        assert(widget_set_input_enabled(widget, true) == BOXRIG_UI_OK);

        /* A held key is suppressed through its release, including after resuming. */
#ifdef BOXRIG_TEST_GTK3
        filter_owner = widget;
        changed_key.type = GDK_KEY_PRESS;
        g_signal_emit_by_name(widget, "key-press-event", &changed_key, &handled);
#else
        g_signal_emit_by_name(native_key, "key-pressed", GDK_KEY_Return, 36, 0, &handled);
#endif
        assert(widget_set_input_enabled(widget, false) == BOXRIG_UI_OK);
        assert(widget_set_input_enabled(widget, true) == BOXRIG_UI_OK);
#ifdef BOXRIG_TEST_GTK3
        commit_on_release = true;
        g_signal_emit_by_name(widget, "key-press-event", &changed_key, &handled);
        changed_key.type = GDK_KEY_RELEASE;
        g_signal_emit_by_name(widget, "key-release-event", &changed_key, &handled);
        commit_on_release = false;
        filter_owner = NULL;
#else
        g_signal_emit_by_name(native_key, "key-pressed", GDK_KEY_Return, 36, 0, &handled);
        g_signal_emit_by_name(native_key, "key-released", GDK_KEY_Return, 36, 0);
#endif
        assert(widget_frame(widget, &frame) == BOXRIG_UI_OK);
        assert(boxrig_terminal_output_peek(terminal, &output) == 0);
#ifdef BOXRIG_TEST_GTK3
        changed_key.type = GDK_KEY_PRESS;
        g_signal_emit_by_name(widget, "key-press-event", &changed_key, &handled);
        changed_key.type = GDK_KEY_RELEASE;
        g_signal_emit_by_name(widget, "key-release-event", &changed_key, &handled);
#else
        g_signal_emit_by_name(native_key, "key-pressed", GDK_KEY_Return, 36, 0, &handled);
        g_signal_emit_by_name(native_key, "key-released", GDK_KEY_Return, 36, 0);
#endif
        assert(widget_frame(widget, &frame) == BOXRIG_UI_OK);
        assert(boxrig_terminal_output_peek(terminal, &output) == 1 && output[0] == '\n');
        assert(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
        assert(widget_quiesce(widget) == BOXRIG_UI_OK);
#ifdef BOXRIG_TEST_GTK3
        gtk_widget_destroy(window);
#endif
        g_object_unref(widget);
        boxrig_ui_model_destroy(model);
        boxrig_terminal_destroy(terminal);
    }
    puts("PASS native GTK UTF-8/Shift_JIS IME, key settings, paste, one-byte chunks and backpressure");
    return 0;
}
