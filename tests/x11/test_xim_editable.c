/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "../editable_fixture.h"
#include "boxrig/x11.h"
#include "boxrig/ui/editable.h"

#include <X11/keysym.h>
#include <X11/Xresource.h>
#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    exit(1); } } while (0)

typedef struct native_context {
    XIMCallback conversion;
    bool destroyed;
} native_context;

typedef struct fixture {
    Display *display;
    Window window;
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_view *view;
    boxrig_ui_editable_document *document;
    boxrig_x11_input_adapter *input;
    char text[128];
    size_t length;
    uint64_t revision;
    size_t calls;
    size_t accepted;
    boxrig_ui_status response;
} fixture;

static native_context native_contexts[128];
static size_t native_count;
static bool advertised = true;
static bool query_rejected;
static bool registration_rejected;
static const char *lookup_text;
static XIMStringConversionCallbackStruct *filtered_conversion;
static size_t allocations_until_failure = SIZE_MAX;

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);

void *__wrap_malloc(size_t size)
{
    if (allocations_until_failure == 0) return NULL;
    if (allocations_until_failure != SIZE_MAX) allocations_until_failure--;
    return __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
    if (allocations_until_failure == 0) return NULL;
    if (allocations_until_failure != SIZE_MAX) allocations_until_failure--;
    return __real_calloc(count, size);
}

XIM __wrap_XOpenIM(Display *display, XrmDatabase db, char *name, char *class_name)
{
    (void)display; (void)db; (void)name; (void)class_name;
    return (XIM)native_contexts;
}

Status __wrap_XCloseIM(XIM im)
{
    (void)im;
    return 1;
}

char *__wrap_XGetIMValues(XIM im, ...)
{
    (void)im;
    va_list arguments;
    va_start(arguments, im);
    const char *name;
    while ((name = va_arg(arguments, const char *)) != NULL) {
        if (strcmp(name, XNQueryInputStyle) == 0) {
            XIMStyles **output = va_arg(arguments, XIMStyles **);
            XIMStyles *styles = calloc(1, sizeof(*styles) + sizeof(XIMStyle));
            CHECK(styles != NULL);
            styles->supported_styles = (XIMStyle *)(styles + 1);
            styles->count_styles = 1;
            styles->supported_styles[0] = XIMPreeditNothing | XIMStatusNothing;
            *output = styles;
        } else {
            CHECK(strcmp(name, XNQueryICValuesList) == 0);
            XIMValuesList **output = va_arg(arguments, XIMValuesList **);
            if (query_rejected) {
                *output = NULL;
                va_end(arguments);
                return XNQueryICValuesList;
            }
            XIMValuesList *values = calloc(1, sizeof(*values) + sizeof(char *));
            CHECK(values != NULL);
            values->count_values = advertised ? 1 : 0;
            values->supported_values = (char **)(values + 1);
            values->supported_values[0] = XNStringConversionCallback;
            *output = values;
        }
    }
    va_end(arguments);
    return NULL;
}

XIC __wrap_XCreateIC(XIM im, ...)
{
    (void)im;
    CHECK(native_count < sizeof(native_contexts) / sizeof(native_contexts[0]));
    native_context *context = &native_contexts[native_count++];
    memset(context, 0, sizeof(*context));
    return (XIC)context;
}

char *__wrap_XSetICValues(XIC ic, ...)
{
    va_list arguments;
    va_start(arguments, ic);
    const char *name = va_arg(arguments, const char *);
    if (strcmp(name, XNStringConversionCallback) == 0) {
        if (registration_rejected) {
            va_end(arguments);
            return XNStringConversionCallback;
        }
        ((native_context *)ic)->conversion = *va_arg(arguments, XIMCallback *);
    }
    va_end(arguments);
    return NULL;
}

void __wrap_XSetICFocus(XIC ic) { (void)ic; }
void __wrap_XUnsetICFocus(XIC ic) { (void)ic; }
void __wrap_XDestroyIC(XIC ic) { ((native_context *)ic)->destroyed = true; }
char *__wrap_Xutf8ResetIC(XIC ic) { (void)ic; return NULL; }

Bool __wrap_XFilterEvent(XEvent *event, Window window)
{
    (void)window;
    if (event->type == KeyPress && filtered_conversion != NULL) {
        native_context *native = &native_contexts[native_count - 1];
        CHECK(native->conversion.callback != NULL);
        native->conversion.callback((XIM)native, native->conversion.client_data,
                                    (XPointer)filtered_conversion);
    }
    return False;
}

int __wrap_Xutf8LookupString(
    XIC ic, XKeyPressedEvent *event, char *buffer, int capacity,
    KeySym *symbol, Status *status)
{
    (void)ic; (void)event;
    size_t length = lookup_text == NULL ? 0 : strlen(lookup_text);
    CHECK(length <= (size_t)capacity);
    if (length != 0) memcpy(buffer, lookup_text, length);
    *symbol = NoSymbol;
    *status = length == 0 ? XLookupNone : XLookupChars;
    return (int)length;
}

static boxrig_ui_status apply_edit(
    const boxrig_ui_editable_replacement_request *request,
    const boxrig_ui_editable_document_snapshot *proposed, void *data)
{
    fixture *f = data;
    f->calls++;
    CHECK(request->document_identity == 19);
    CHECK(request->base_document_revision == f->revision);
    CHECK(request->range.offset >= 100);
    CHECK(request->range.offset + request->range.length <= 100 + f->length);
    if (f->response != BOXRIG_UI_OK) return f->response;
    CHECK(proposed->context_length < sizeof(f->text));
    memcpy(f->text, proposed->context, proposed->context_length);
    f->text[proposed->context_length] = '\0';
    f->length = proposed->context_length;
    f->revision = proposed->document_revision;
    f->accepted++;
    return BOXRIG_UI_OK;
}

static void publish(fixture *f, const char *text, size_t cursor, size_t anchor)
{
    strcpy(f->text, text);
    f->length = strlen(text);
    f->revision++;
    boxrig_ui_editable_context_input input = {
        19, f->revision, f->text, f->length, 100, 100 + cursor, 100 + anchor};
    CHECK(boxrig_ui_editable_document_publish_context(f->document, &input) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_input_adapter_retry(f->input) == 0);
}

static void focus(fixture *f, bool focused)
{
    XEvent event = {0};
    event.xfocus.type = focused ? FocusIn : FocusOut;
    event.xfocus.display = f->display;
    event.xfocus.window = f->window;
    CHECK(boxrig_x11_input_adapter_process_event(f->input, &event) == 1);
}

static void create(fixture *f)
{
    memset(f, 0, sizeof(*f));
    f->response = BOXRIG_UI_OK;
    f->display = XOpenDisplay(NULL);
    CHECK(f->display != NULL);
    f->window = XCreateSimpleWindow(f->display, DefaultRootWindow(f->display),
                                   0, 0, 80, 60, 0, 0, 0);
    CHECK(f->window != None);
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.output_capacity = 16;
    CHECK(boxrig_terminal_create(&config, &f->terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(f->terminal, &f->model) == BOXRIG_UI_OK);
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    CHECK(boxrig_ui_view_create(f->model, &view_config, &f->view) == BOXRIG_UI_OK);
    f->document = boxrig_ui_view_editable_document(f->view);
    CHECK(boxrig_x11_input_adapter_create(f->display, f->window, f->window,
              f->terminal, 10, 20, 1024, 128, &f->input) == 0);
    CHECK(boxrig_x11_input_adapter_set_editable_document(f->input, f->document) == 0);
    CHECK(boxrig_ui_editable_document_set_apply_handler(f->document, apply_edit, f) == BOXRIG_UI_OK);
    focus(f, true);
}

static void destroy(fixture *f)
{
    const uint8_t *output;
    CHECK(boxrig_terminal_output_peek(f->terminal, &output) == 0);
    boxrig_x11_input_adapter_destroy(f->input);
    boxrig_ui_view_destroy(f->view);
    boxrig_ui_model_destroy(f->model);
    boxrig_terminal_destroy(f->terminal);
    XDestroyWindow(f->display, f->window);
    XCloseDisplay(f->display);
}

static XIMStringConversionText *convert(
    fixture *f, short position, XIMCaretDirection direction,
    unsigned short factor, XIMStringConversionOperation operation)
{
    (void)f;
    native_context *native = &native_contexts[native_count - 1];
    CHECK(!native->destroyed && native->conversion.callback != NULL);
    XIMStringConversionCallbackStruct request = {
        (XIMStringConversionPosition)position, direction, operation, factor, NULL};
    native->conversion.callback((XIM)native, native->conversion.client_data, (XPointer)&request);
    return request.text;
}

static void expect_text(XIMStringConversionText *text, const wchar_t *expected)
{
    CHECK(text != NULL);
    CHECK(text->encoding_is_wchar);
    CHECK(text->length == wcslen(expected));
    CHECK(text->feedback == NULL);
    CHECK(wmemcmp(text->string.wcs, expected, text->length) == 0);
    /* Xlib section 13.5.7.3 transfers the string/feedback allocations to the
     * input method, while the descriptor itself remains client-owned. */
    XFree(text->string.wcs);
}

static void commit(fixture *f, const char *text)
{
    XEvent event = {0};
    event.xkey.type = KeyPress;
    event.xkey.display = f->display;
    event.xkey.window = f->window;
    event.xkey.keycode = XKeysymToKeycode(f->display, XK_a);
    lookup_text = text;
    CHECK(boxrig_x11_input_adapter_process_event(f->input, &event) == 1);
    lookup_text = NULL;
    event.type = KeyRelease;
    CHECK(boxrig_x11_input_adapter_process_event(f->input, &event) == 1);
}

static void test_capabilities(void)
{
    for (int mode = 0; mode < 4; mode++) {
        advertised = mode != 1;
        query_rejected = mode == 2;
        registration_rejected = mode == 3;
        fixture f;
        create(&f);
        CHECK(boxrig_x11_input_adapter_string_conversion_supported(f.input) == (mode == 0));
        if (mode == 0)
            CHECK(convert(&f, 0, XIMBackwardChar, 1, XIMStringConversionRetrieval) == NULL);
        commit(&f, "語");
        const uint8_t *output;
        CHECK(boxrig_terminal_output_peek(f.terminal, &output) == 3);
        CHECK(memcmp(output, "語", 3) == 0);
        CHECK(boxrig_terminal_output_consume(f.terminal, 3) == BOXRIG_STATUS_OK);
        destroy(&f);
    }
    advertised = true;
    query_rejected = registration_rejected = false;
    puts("PASS XIM advertised, unsupported, rejected capabilities and terminal fallback");
}

static void test_shared_document_states(void)
{
    for (size_t index = 0; index < sizeof(boxrig_editable_cases) / sizeof(boxrig_editable_cases[0]); index++) {
        fixture f;
        create(&f);
        CHECK(boxrig_x11_input_adapter_string_conversion_supported(f.input));
        CHECK(boxrig_ui_editable_document_set_apply_handler(f.document,
            boxrig_editable_cases[index].synchronous_edit_available ? apply_edit : NULL,
            boxrig_editable_cases[index].synchronous_edit_available ? &f : NULL) == BOXRIG_UI_OK);
        strcpy(f.text, boxrig_editable_fixture.text);
        if (boxrig_editable_cases[index].context_available)
            publish(&f, boxrig_editable_fixture.text, boxrig_editable_fixture.cursor, boxrig_editable_fixture.anchor);
        CHECK(boxrig_x11_input_adapter_retry(f.input) == 0);
        boxrig_ui_editable_capabilities caps = boxrig_ui_editable_document_capabilities(f.document);
        CHECK(caps.context_available == boxrig_editable_cases[index].context_available);
        CHECK(caps.synchronous_edit_available == boxrig_editable_cases[index].synchronous_edit_available);
        CHECK(!caps.reconversion_available && !caps.busy);
        XIMStringConversionText *text = convert(&f, -3, XIMForwardChar, 4, XIMStringConversionRetrieval);
        if (caps.context_available) {
            expect_text(text, L"前かな後");
            text = convert(&f, 0, XIMBackwardChar, 1, XIMStringConversionSubstitution);
            if (caps.synchronous_edit_available) expect_text(text, L"な");
            else CHECK(text == NULL);
        } else CHECK(text == NULL);
        commit(&f, boxrig_editable_fixture.commit);
        CHECK(strcmp(f.text, boxrig_editable_cases[index].after_synchronous_edit) == 0);
        const uint8_t *bytes;
        size_t length = boxrig_terminal_output_peek(f.terminal, &bytes);
        CHECK(length == boxrig_editable_cases[index].terminal_bytes);
        if (length != 0) {
            CHECK(memcmp(bytes, boxrig_editable_fixture.commit, length) == 0);
            CHECK(boxrig_terminal_output_consume(f.terminal, length) == BOXRIG_STATUS_OK);
        }
        printf("PASS XIM shared editable state=%s\n", boxrig_editable_cases[index].name);
        destroy(&f);
    }
}

static void test_retrieve_and_substitute(void)
{
    fixture f;
    create(&f);
    publish(&f, "前かな後", 9, 9);
    expect_text(convert(&f, -2, XIMForwardChar, 2, XIMStringConversionRetrieval), L"かな");
    CHECK(f.calls == 0 && strcmp(f.text, "前かな後") == 0);
    expect_text(convert(&f, 0, XIMBackwardChar, 1, XIMStringConversionSubstitution), L"な");
    CHECK(f.accepted == 1 && strcmp(f.text, "前か後") == 0);
    expect_text(convert(&f, -1, XIMForwardChar, 2, XIMStringConversionRetrieval), L"か後");
    commit(&f, "語");
    CHECK(f.accepted == 2 && strcmp(f.text, "前か語後") == 0);
    publish(&f, "前😀e\xcc\x81\n後", 10, 3);
    expect_text(convert(&f, 0, XIMBackwardChar, 1, XIMStringConversionSubstitution), L"\u0301");
    CHECK(strcmp(f.text, "前😀e\n後") == 0);
    boxrig_ui_editable_document_snapshot snapshot;
    CHECK(boxrig_ui_editable_document_get_snapshot(f.document, &snapshot));
    CHECK(snapshot.cursor == 108 && snapshot.anchor == 103);
    expect_text(convert(&f, -2, XIMForwardChar, 3, XIMStringConversionRetrieval), L"😀e\n");
    commit(&f, "日本");
    CHECK(strcmp(f.text, "前日本\n後") == 0);
    publish(&f, "", 0, 0);
    expect_text(convert(&f, 0, XIMForwardChar, 0, XIMStringConversionRetrieval), L"");
    commit(&f, "あ");
    CHECK(strcmp(f.text, "あ") == 0);
    destroy(&f);
    puts("PASS XIM scalar retrieval, synchronous deletion, selection and native commits");
}

static void test_rejection_and_retry(void)
{
    fixture f;
    create(&f);
    publish(&f, "前かな後", 9, 9);
    f.response = BOXRIG_UI_BUSY;
    CHECK(boxrig_x11_input_adapter_commit_utf8(f.input, "語", 3) == 0);
    CHECK(boxrig_x11_input_adapter_blocked(f.input));
    CHECK(f.calls == 1 && f.accepted == 0);
    f.response = BOXRIG_UI_OK;
    CHECK(boxrig_x11_input_adapter_retry(f.input) == 0);
    CHECK(!boxrig_x11_input_adapter_blocked(f.input));
    CHECK(f.calls == 2 && f.accepted == 1 && strcmp(f.text, "前かな語後") == 0);
    CHECK(boxrig_x11_input_adapter_retry(f.input) == 0 && f.accepted == 1);

    for (int mode = 0; mode < 4; mode++) {
        publish(&f, "前かな後", 9, 9);
        f.response = mode == 0 ? BOXRIG_UI_BUSY :
                     mode == 1 ? BOXRIG_UI_UNAVAILABLE : BOXRIG_UI_STALE;
        if (mode == 3) {
            CHECK(boxrig_ui_editable_document_set_apply_handler(f.document, NULL, NULL) == BOXRIG_UI_OK);
            CHECK(boxrig_x11_input_adapter_retry(f.input) == 0);
        }
        size_t accepted = f.accepted;
        XIMStringConversionCallbackStruct request = {
            0, XIMBackwardChar, XIMStringConversionSubstitution, 1, NULL};
        filtered_conversion = &request;
        commit(&f, "語");
        filtered_conversion = NULL;
        CHECK(request.text == NULL);
        CHECK(f.accepted == accepted && strcmp(f.text, "前かな後") == 0);
        f.response = BOXRIG_UI_OK;
        CHECK(boxrig_ui_editable_document_set_apply_handler(f.document, apply_edit, &f) == BOXRIG_UI_OK);
        CHECK(boxrig_x11_input_adapter_retry(f.input) == 0);
        expect_text(convert(&f, 0, XIMBackwardChar, 1, XIMStringConversionRetrieval), L"な");
    }
    destroy(&f);
    puts("PASS XIM refusal blocks the following commit and BUSY commit retries once");
}

static void test_stale_focus_and_limits(void)
{
    fixture f;
    create(&f);
    publish(&f, "前かな後", 9, 9);
    native_context *old = &native_contexts[native_count - 1];
    boxrig_ui_editable_context_input moved = {19, f.revision, f.text, f.length, 100, 106, 106};
    CHECK(boxrig_ui_editable_document_publish_context(f.document, &moved) == BOXRIG_UI_OK);
    CHECK(convert(&f, 0, XIMBackwardChar, 1, XIMStringConversionSubstitution) == NULL);
    CHECK(f.calls == 0);
    CHECK(boxrig_x11_input_adapter_retry(f.input) == 0);
    XIMStringConversionCallbackStruct late = {
        0, XIMBackwardChar, XIMStringConversionSubstitution, 1, NULL};
    old->conversion.callback((XIM)old, old->conversion.client_data, (XPointer)&late);
    CHECK(late.text == NULL && f.calls == 0);
    expect_text(convert(&f, 0, XIMBackwardChar, 1, XIMStringConversionRetrieval), L"か");
    CHECK(convert(&f, SHRT_MIN, XIMForwardChar, 1, XIMStringConversionRetrieval) == NULL);
    CHECK(convert(&f, 0, XIMForwardChar, USHRT_MAX, XIMStringConversionRetrieval) == NULL);
    CHECK(convert(&f, 0, XIMLineEnd, 1, XIMStringConversionRetrieval) == NULL);
    CHECK(convert(&f, 0, XIMForwardChar, 1, 99) == NULL);
    CHECK(boxrig_x11_input_adapter_retry(f.input) == 0);
    f.response = BOXRIG_UI_BUSY;
    CHECK(boxrig_x11_input_adapter_commit_utf8(f.input, "語", 3) == 0);
    CHECK(boxrig_x11_input_adapter_blocked(f.input));
    focus(&f, false);
    CHECK(!boxrig_x11_input_adapter_blocked(f.input));
    CHECK(convert(&f, 0, XIMBackwardChar, 1, XIMStringConversionSubstitution) == NULL);
    f.response = BOXRIG_UI_OK;
    focus(&f, true);
    CHECK(boxrig_x11_input_adapter_retry(f.input) == 0);
    CHECK(f.accepted == 0 && strcmp(f.text, "前かな後") == 0);
    CHECK(boxrig_x11_input_adapter_quiesce(f.input) == 0);
    CHECK(convert(&f, 0, XIMBackwardChar, 1, XIMStringConversionSubstitution) == NULL);
    destroy(&f);
    puts("PASS XIM stale cursor, late callback, bounds, focus and quiescence");
}

static void test_allocation_failure(void)
{
    fixture f;
    create(&f);
    bool succeeded = false;
    for (size_t failure = 0; failure < 20; failure++) {
        publish(&f, "前かな後", 9, 9);
        size_t calls = f.calls;
        allocations_until_failure = failure;
        XIMStringConversionText *text = convert(
            &f, 0, XIMBackwardChar, 1, XIMStringConversionSubstitution);
        allocations_until_failure = SIZE_MAX;
        if (text != NULL) {
            expect_text(text, L"な");
            CHECK(f.calls == calls + 1 && strcmp(f.text, "前か後") == 0);
            succeeded = true;
            break;
        }
        CHECK(f.calls == calls && strcmp(f.text, "前かな後") == 0);
        CHECK(boxrig_x11_input_adapter_retry(f.input) == 0);
    }
    CHECK(succeeded);
    destroy(&f);
    puts("PASS XIM prepares all response and owner storage before document acceptance");
}

static void test_window_and_cancellation(void)
{
    fixture f;
    create(&f);
    char large[9001];
    memset(large, 'a', sizeof(large) - 1);
    large[9000] = '\0';
    boxrig_ui_editable_context_input input = {19, 1, large, 9000, 100, 4600, 4600};
    CHECK(boxrig_ui_editable_document_publish_context(f.document, &input) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_input_adapter_retry(f.input) == 0);
    expect_text(convert(&f, -2, XIMForwardChar, 4, XIMStringConversionRetrieval), L"aaaa");
    CHECK(convert(&f, -4500, XIMForwardChar, 1, XIMStringConversionRetrieval) == NULL);
    input.cursor = 100;
    input.anchor = 4102;
    CHECK(boxrig_ui_editable_document_publish_context(f.document, &input) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_input_adapter_retry(f.input) == 0);
    CHECK(convert(&f, 0, XIMForwardChar, 1, XIMStringConversionRetrieval) == NULL);
    input.document_revision = 2;
    input.context = "a\0b";
    input.context_length = 3;
    input.cursor = input.anchor = 102;
    CHECK(boxrig_ui_editable_document_publish_context(f.document, &input) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_input_adapter_retry(f.input) == 0);
    CHECK(convert(&f, 0, XIMBackwardChar, 1, XIMStringConversionSubstitution) == NULL);
    CHECK(f.calls == 0);

    f.revision = 2;
    publish(&f, "前かな後", 9, 9);
    CHECK(boxrig_x11_input_adapter_preedit_start(f.input) == 0);
    f.response = BOXRIG_UI_BUSY;
    CHECK(boxrig_x11_input_adapter_commit_utf8(f.input, "語", 3) == 0);
    CHECK(boxrig_x11_input_adapter_blocked(f.input));
    CHECK(boxrig_x11_input_adapter_preedit_cancel(f.input) == 0);
    CHECK(!boxrig_x11_input_adapter_blocked(f.input));
    f.response = BOXRIG_UI_OK;
    CHECK(boxrig_x11_input_adapter_commit_utf8(f.input, "遅", 3) == 0);
    CHECK(f.accepted == 0 && strcmp(f.text, "前かな後") == 0);
    CHECK(boxrig_x11_input_adapter_retry(f.input) == 0);
    expect_text(convert(&f, 0, XIMBackwardChar, 1, XIMStringConversionRetrieval), L"な");

    f.response = BOXRIG_UI_BUSY;
    CHECK(boxrig_x11_input_adapter_commit_utf8(f.input, "語", 3) == 0);
    input = (boxrig_ui_editable_context_input){19, f.revision, f.text, f.length, 100, 103, 103};
    CHECK(boxrig_ui_editable_document_publish_context(f.document, &input) == BOXRIG_UI_OK);
    f.response = BOXRIG_UI_OK;
    CHECK(boxrig_x11_input_adapter_retry(f.input) == 0);
    CHECK(f.accepted == 0);
    commit(&f, "次");
    CHECK(strcmp(f.text, "前次かな後") == 0);
    destroy(&f);
    puts("PASS XIM bounded windows, unsupported NUL, cancellation and pending generation changes");
}

static void test_pending_commit_destinations(void)
{
    fixture f;
    create(&f);
    CHECK(boxrig_terminal_send_text(f.terminal, "0123456789012345", 16) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_commit_utf8(f.input, "語", 3) == 0);
    CHECK(boxrig_x11_input_adapter_blocked(f.input));
    publish(&f, "文書", 6, 6);
    CHECK(boxrig_terminal_output_consume(f.terminal, 16) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_retry(f.input) == 0);
    const uint8_t *output;
    CHECK(boxrig_terminal_output_peek(f.terminal, &output) == 3);
    CHECK(memcmp(output, "語", 3) == 0);
    CHECK(boxrig_terminal_output_consume(f.terminal, 3) == BOXRIG_STATUS_OK);
    CHECK(f.accepted == 0);
    f.response = BOXRIG_UI_BUSY;
    CHECK(boxrig_x11_input_adapter_commit_utf8(f.input, "次", 3) == 0);
    CHECK(boxrig_x11_input_adapter_blocked(f.input));
    CHECK(boxrig_x11_input_adapter_set_editable_document(f.input, NULL) == 0);
    CHECK(!boxrig_x11_input_adapter_blocked(f.input));
    f.response = BOXRIG_UI_OK;
    CHECK(boxrig_x11_input_adapter_retry(f.input) == 0);
    CHECK(f.accepted == 0 && strcmp(f.text, "文書") == 0);
    destroy(&f);
    puts("PASS XIM queued terminal and document commits retain their destinations");
}

static void test_component_document(void)
{
    fixture f = {0};
    f.response = BOXRIG_UI_OK;
    f.display = XOpenDisplay(NULL);
    CHECK(f.display != NULL);
    Window parent = XCreateSimpleWindow(f.display, DefaultRootWindow(f.display),
                                        0, 0, 320, 180, 0, 0, 0);
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    terminal_config.rows = 3;
    terminal_config.columns = 12;
    CHECK(boxrig_terminal_create(&terminal_config, &f.terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(f.terminal, &f.model) == BOXRIG_UI_OK);
    boxrig_x11_terminal_component_config config = boxrig_x11_terminal_component_config_default();
    config.view.blink.text_enabled = config.view.blink.cursor_enabled = false;
    boxrig_x11_terminal_component *component = NULL;
    CHECK(boxrig_x11_terminal_component_create_child(f.display, parent, f.model,
              &config, &component) == BOXRIG_UI_OK);
    f.window = boxrig_x11_terminal_component_owned_window(component);
    f.view = boxrig_x11_terminal_component_view(component);
    f.document = boxrig_ui_view_editable_document(f.view);
    CHECK(boxrig_ui_editable_document_set_apply_handler(f.document, apply_edit, &f) == BOXRIG_UI_OK);
    strcpy(f.text, "前かな後");
    f.length = strlen(f.text);
    f.revision = 1;
    boxrig_ui_editable_context_input input = {19, 1, f.text, f.length, 100, 109, 109};
    CHECK(boxrig_ui_editable_document_publish_context(f.document, &input) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    XEvent event = {0};
    event.xfocus.type = FocusIn;
    event.xfocus.display = f.display;
    event.xfocus.window = f.window;
    event.xfocus.mode = NotifyNormal;
    CHECK(boxrig_x11_terminal_component_dispatch_event(component, &event) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    expect_text(convert(&f, 0, XIMBackwardChar, 1, XIMStringConversionSubstitution), L"な");
    event.xkey.type = KeyPress;
    event.xkey.display = f.display;
    event.xkey.window = f.window;
    event.xkey.keycode = XKeysymToKeycode(f.display, XK_a);
    lookup_text = "語";
    CHECK(boxrig_x11_terminal_component_dispatch_event(component, &event) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    lookup_text = NULL;
    CHECK(f.accepted == 2 && strcmp(f.text, "前か語後") == 0);
    const uint8_t *output;
    CHECK(boxrig_terminal_output_peek(f.terminal, &output) == 0);
    boxrig_x11_terminal_component_destroy(component);
    boxrig_ui_model_destroy(f.model);
    boxrig_terminal_destroy(f.terminal);
    XDestroyWindow(f.display, parent);
    cairo_debug_reset_static_data();
    XCloseDisplay(f.display);
    FcFini();
    puts("PASS X11 component connects its view document to native XIM input");
}

int main(void)
{
    CHECK(setlocale(LC_CTYPE, "C.UTF-8") != NULL);
    test_capabilities();
    test_shared_document_states();
    test_retrieve_and_substitute();
    test_rejection_and_retry();
    test_stale_focus_and_limits();
    test_allocation_failure();
    test_window_and_cancellation();
    test_pending_commit_destinations();
    test_component_document();
    return 0;
}
