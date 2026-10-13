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
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Capture the native input objects while keeping their normal ownership. */
static GtkIMContext *native_im;
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
        tc.enter_newline = BOXRIG_NEWLINE_LF;
        tc.paste_newline = BOXRIG_NEWLINE_CRLF;
        tc.key_mappings = &mapping;
        tc.key_mapping_count = 1;
        tc.output_capacity = 6;
        widget_config config = config_default();
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
            event.type = GDK_KEY_RELEASE;
            g_signal_emit_by_name(widget, "key-release-event", &event, &handled);
#else
            g_signal_emit_by_name(native_key, "key-pressed", keys[index], codes[index], 0, &handled);
            assert(handled);
            g_signal_emit_by_name(native_key, "key-released", keys[index], codes[index], 0);
#endif
        }
        assert(widget_frame(widget, &frame) == BOXRIG_UI_OK);
        const uint8_t key_expected[] = {0x1b, 'U', 0, '\n'};
        assert(boxrig_terminal_output_peek(terminal, &output) == sizeof(key_expected));
        assert(memcmp(output, key_expected, sizeof(key_expected)) == 0);
        assert(boxrig_terminal_output_consume(terminal, sizeof(key_expected)) == BOXRIG_STATUS_OK);
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
