/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "../composition_fixture.h"
#include "boxrig/x11.h"

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <locale.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

typedef struct routed_key_trace {
    boxrig_key_action actions[4];
    boxrig_key_event events[4];
    size_t count;
} routed_key_trace;

typedef struct routed_mouse_trace {
    boxrig_mouse_event events[16];
    boxrig_ui_hit_test_result hits[16];
    size_t count;
} routed_mouse_trace;

static Atom intercepted_input_method_message = None;
static size_t input_method_filter_calls;
static Window input_method_filter_window = None;
static KeyCode intercepted_lookup_keycode;
static KeyCode intercepted_lookup_both_keycode;
static KeyCode intercepted_preedit_done_keycode;
static boxrig_x11_input_adapter *intercepted_preedit_done_adapter;
static bool intercepted_preedit_done_failed;
static KeyCode intercepted_empty_preedit_keycode;
static boxrig_x11_input_adapter *intercepted_empty_preedit_adapter;
static bool intercepted_empty_preedit_failed;
static KeyCode intercepted_passthrough_keycode;
static KeyCode intercepted_filtered_keycode;
static size_t candidate_spot_set_calls;
static bool maximum_request_size_overridden;
static long maximum_request_size_override;
static long extended_maximum_request_size_override;
static bool keymap_overridden;
static KeyCode overridden_keycode;
static KeySym overridden_keysyms[XkbNumKbdGroups][2];

Bool __real_XFilterEvent(XEvent *event, Window window);
int __real_Xutf8LookupString(
    XIC input_context,
    XKeyPressedEvent *event,
    char *buffer,
    int buffer_length,
    KeySym *symbol,
    Status *status);
long __real_XMaxRequestSize(Display *display);
long __real_XExtendedMaxRequestSize(Display *display);
KeySym __real_XkbKeycodeToKeysym(
    Display *display,
    KeyCode keycode,
    int group,
    int level);

KeySym __wrap_XkbKeycodeToKeysym(
    Display *display,
    KeyCode keycode,
    int group,
    int level)
{
    if (keymap_overridden && keycode == overridden_keycode &&
        group >= 0 && group < XkbNumKbdGroups &&
        level >= 0 && level < 2) {
        return overridden_keysyms[group][level];
    }
    return __real_XkbKeycodeToKeysym(
        display,
        keycode,
        group,
        level);
}

long __wrap_XMaxRequestSize(Display *display)
{
    if (maximum_request_size_overridden) {
        return maximum_request_size_override;
    }
    return __real_XMaxRequestSize(display);
}

long __wrap_XExtendedMaxRequestSize(Display *display)
{
    if (maximum_request_size_overridden) {
        return extended_maximum_request_size_override;
    }
    return __real_XExtendedMaxRequestSize(display);
}

char *__wrap_XSetICValues(XIC input_context, ...)
{
    (void)input_context;
    candidate_spot_set_calls++;
    return NULL;
}

Bool __wrap_XFilterEvent(XEvent *event, Window window)
{
    if (event != NULL &&
        (event->type == KeyPress || event->type == KeyRelease) &&
        event->xkey.keycode == intercepted_filtered_keycode) {
        return True;
    }
    if (event != NULL && event->type == KeyPress &&
        event->xkey.keycode == intercepted_passthrough_keycode) {
        return False;
    }
    if (event != NULL && event->type == KeyPress &&
        event->xkey.keycode == intercepted_empty_preedit_keycode) {
        XIMPreeditDrawCallbackStruct draw;

        memset(&draw, 0, sizeof(draw));
        draw.chg_length = 1;
        if (intercepted_empty_preedit_adapter == NULL ||
            boxrig_x11_input_adapter_preedit_draw(
                intercepted_empty_preedit_adapter,
                &draw) == -1) {
            intercepted_empty_preedit_failed = true;
        }
        return False;
    }
    if (event != NULL && event->type == KeyPress &&
        event->xkey.keycode == intercepted_preedit_done_keycode) {
        if (intercepted_preedit_done_adapter == NULL ||
            boxrig_x11_input_adapter_preedit_done(
                intercepted_preedit_done_adapter) == -1) {
            intercepted_preedit_done_failed = true;
        }
        return False;
    }
    if (event != NULL && event->type == ClientMessage &&
        event->xclient.message_type == intercepted_input_method_message) {
        input_method_filter_calls++;
        input_method_filter_window = window;
        return window == None ? True : False;
    }
    if (event != NULL &&
        (event->type == KeyPress || event->type == KeyRelease) &&
        (event->xkey.keycode == intercepted_lookup_keycode ||
         event->xkey.keycode == intercepted_lookup_both_keycode)) {
        return False;
    }
    return __real_XFilterEvent(event, window);
}

int __wrap_Xutf8LookupString(
    XIC input_context,
    XKeyPressedEvent *event,
    char *buffer,
    int buffer_length,
    KeySym *symbol,
    Status *status)
{
    if (event != NULL &&
        event->keycode == intercepted_lookup_both_keycode) {
        if (buffer_length < 1) {
            *status = XBufferOverflow;
            return 1;
        }
        buffer[0] = '\b';
        *symbol = XK_BackSpace;
        *status = XLookupBoth;
        return 1;
    }
    if (event != NULL && event->keycode == intercepted_lookup_keycode) {
        if ((size_t)buffer_length <
            boxrig_composition_fixture.commit_utf8_length) {
            *status = XBufferOverflow;
            return (int)boxrig_composition_fixture.commit_utf8_length;
        }
        memcpy(
            buffer,
            boxrig_composition_fixture.commit_utf8,
            boxrig_composition_fixture.commit_utf8_length);
        *symbol = NoSymbol;
        *status = XLookupChars;
        return (int)boxrig_composition_fixture.commit_utf8_length;
    }
    return __real_Xutf8LookupString(
        input_context,
        event,
        buffer,
        buffer_length,
        symbol,
        status);
}

static boxrig_input_key_route route_control_a(
    const boxrig_key_event *event,
    void *opaque_context)
{
    routed_key_trace *trace = opaque_context;

    if (event->key != BOXRIG_KEY_NONE || event->codepoint != 'a' ||
        (event->modifiers & BOXRIG_MODIFIER_CONTROL) == 0) {
        return BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
    }
    if (trace->count >= sizeof(trace->actions) / sizeof(trace->actions[0])) {
        return BOXRIG_INPUT_KEY_ROUTE_ERROR;
    }
    trace->events[trace->count] = *event;
    trace->actions[trace->count++] = event->action;
    return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
}

static boxrig_input_key_route capture_key_identity(
    const boxrig_key_event *event,
    void *opaque_context)
{
    routed_key_trace *trace = opaque_context;

    if (trace->count >= sizeof(trace->events) / sizeof(trace->events[0])) {
        return BOXRIG_INPUT_KEY_ROUTE_ERROR;
    }
    trace->events[trace->count++] = *event;
    return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
}

static boxrig_input_mouse_route route_left_drag(
    const boxrig_mouse_event *event,
    const boxrig_ui_hit_test_result *hit,
    void *opaque_context)
{
    routed_mouse_trace *trace = opaque_context;

    if (hit == NULL ||
        trace->count >= sizeof(trace->events) / sizeof(trace->events[0])) {
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    if (event->action == BOXRIG_MOUSE_PRESS &&
        event->button == BOXRIG_MOUSE_BUTTON_LEFT) {
        trace->events[trace->count] = *event;
        trace->hits[trace->count++] = *hit;
        return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
    }
    if (trace->count != 0 && trace->count < 3) {
        trace->events[trace->count] = *event;
        trace->hits[trace->count++] = *hit;
    }
    return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
}

static bool take_window_event(
    Display *display,
    Window window,
    int type,
    XEvent *event_out)
{
    XEvent event;

    XSync(display, False);
    while (XCheckTypedWindowEvent(display, window, type, &event)) {
        *event_out = event;
        return true;
    }
    return false;
}

static bool take_property_event(
    Display *display,
    Window window,
    int state,
    XEvent *event_out)
{
    XEvent event;

    XSync(display, False);
    while (XCheckTypedWindowEvent(
        display,
        window,
        PropertyNotify,
        &event)) {
        if (event.xproperty.state == state) {
            *event_out = event;
            return true;
        }
    }
    return false;
}

static bool take_atom_property_event(
    Display *display,
    Window window,
    Atom atom,
    int state,
    XEvent *event_out)
{
    XEvent event;

    XSync(display, False);
    while (XCheckTypedWindowEvent(
        display,
        window,
        PropertyNotify,
        &event)) {
        if (event.xproperty.atom == atom &&
            event.xproperty.state == state) {
            *event_out = event;
            return true;
        }
    }
    return false;
}

static bool test_xim_protocol_events_are_filtered(void)
{
    Display *display;
    Window window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_x11_input_adapter *adapter = NULL;
    XEvent event;

    display = XOpenDisplay(NULL);
    CHECK(display != NULL);
    window = XCreateSimpleWindow(
        display,
        RootWindow(display, DefaultScreen(display)),
        0,
        0,
        40,
        60,
        0,
        0,
        0);
    CHECK(window != None);
    config.rows = 3;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_create(
              display,
              window,
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);

    intercepted_input_method_message = XInternAtom(
        display,
        "_BOXRIG_TEST_XIM_PROTOCOL",
        False);
    CHECK(intercepted_input_method_message != None);
    input_method_filter_calls = 0;
    input_method_filter_window = window;
    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.display = display;
    event.xclient.window = window;
    event.xclient.message_type = intercepted_input_method_message;
    event.xclient.format = 8;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(input_method_filter_calls == 1);
    CHECK(input_method_filter_window == None);

    intercepted_input_method_message = None;
    boxrig_x11_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return true;
}

static bool test_xim_functional_key_commits_text(void)
{
    Display *display;
    Window window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_x11_input_adapter *adapter = NULL;
    boxrig_composition *composition;
    boxrig_composition_view view;
    XEvent event;
    const uint8_t *output = NULL;
    size_t output_length;

    display = XOpenDisplay(NULL);
    CHECK(display != NULL);
    window = XCreateSimpleWindow(
        display,
        RootWindow(display, DefaultScreen(display)),
        0,
        0,
        40,
        60,
        0,
        0,
        0);
    CHECK(window != None);
    config.rows = 3;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_create(
              display,
              window,
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    composition = boxrig_x11_input_adapter_composition(adapter);
    CHECK(composition != NULL);
    CHECK(boxrig_x11_input_adapter_preedit_start(adapter) == 0);

    intercepted_lookup_keycode = XKeysymToKeycode(display, XK_Return);
    CHECK(intercepted_lookup_keycode != 0);
    memset(&event, 0, sizeof(event));
    event.xkey.type = KeyPress;
    event.xkey.display = display;
    event.xkey.window = window;
    event.xkey.root = RootWindow(display, DefaultScreen(display));
    event.xkey.keycode = intercepted_lookup_keycode;
    event.xkey.time = CurrentTime;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == boxrig_composition_fixture.commit_utf8_length);
    CHECK(memcmp(
              output,
              boxrig_composition_fixture.commit_utf8,
              output_length) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active);
    CHECK(view.last_event == BOXRIG_COMPOSITION_COMMIT);

    event.xkey.type = KeyRelease;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == output_length);

    intercepted_lookup_keycode = 0;
    boxrig_x11_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return true;
}

static bool test_application_key_routing_captures_the_key_series(void)
{
    Display *display;
    Window window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_x11_input_adapter *adapter = NULL;
    routed_key_trace trace;
    XEvent event;
    KeyCode keycode;
    const uint8_t mode[] = "\x1b[=27u";
    const uint8_t expected[] = "\x1b[98;;98u";
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t output_length;

    memset(&trace, 0, sizeof(trace));
    display = XOpenDisplay(NULL);
    CHECK(display != NULL);
    window = XCreateSimpleWindow(
        display,
        RootWindow(display, DefaultScreen(display)),
        0,
        0,
        40,
        60,
        0,
        0,
        0);
    CHECK(window != None);
    config.rows = 3;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_create(
              display,
              window,
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    CHECK(boxrig_x11_input_adapter_set_key_router(
              adapter,
              route_control_a,
              &trace) == 0);
    boxrig_input_config replacement = boxrig_input_config_default();
    CHECK(boxrig_x11_input_adapter_set_input_config(adapter, &replacement) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              mode,
              sizeof(mode) - 1,
              &consumed) == BOXRIG_STATUS_OK);

    memset(&event, 0, sizeof(event));
    event.xkey.display = display;
    event.xkey.window = window;
    event.xkey.root = RootWindow(display, DefaultScreen(display));
    event.xkey.same_screen = True;
    event.xkey.keycode = XKeysymToKeycode(display, XK_a);
    event.xkey.state = ControlMask;
    event.xkey.type = KeyPress;
    event.xkey.time = 1;
    CHECK(event.xkey.keycode != 0);
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(boxrig_x11_input_adapter_set_input_config(adapter, &replacement) == BOXRIG_UI_BUSY);
    event.xkey.time = 2;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    event.xkey.type = KeyRelease;
    event.xkey.time = 3;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(boxrig_x11_input_adapter_set_input_config(adapter, &replacement) == BOXRIG_UI_OK);
    CHECK(trace.count == 3);
    CHECK(trace.actions[0] == BOXRIG_KEY_PRESS);
    CHECK(trace.actions[1] == BOXRIG_KEY_REPEAT);
    CHECK(trace.actions[2] == BOXRIG_KEY_RELEASE);
    CHECK(trace.events[0].codepoint == 'a');
    CHECK(trace.events[0].shifted_codepoint == 'A');
    CHECK(trace.events[0].base_layout_codepoint == 'a');
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    event.xkey.type = KeyPress;
    event.xkey.time = 4;
    event.xkey.state = 0;
    keycode = XKeysymToKeycode(display, XK_b);
    CHECK(keycode != 0);
    event.xkey.keycode = keycode;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, output_length) == 0);
    CHECK(boxrig_x11_input_adapter_set_cell_size(adapter, 5, 10) == 0);
    CHECK(boxrig_x11_input_adapter_set_cell_size(adapter, 0, 10) == -1);

    boxrig_x11_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return true;
}

static bool test_xkb_identity_distinguishes_active_and_base_layouts(void)
{
    Display *display;
    Window window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_x11_input_adapter *adapter = NULL;
    routed_key_trace trace;
    XEvent event;
    KeyCode keycode;

    memset(&trace, 0, sizeof(trace));
    display = XOpenDisplay(NULL);
    CHECK(display != NULL);
    window = XCreateSimpleWindow(
        display,
        RootWindow(display, DefaultScreen(display)),
        0,
        0,
        40,
        60,
        0,
        0,
        0);
    CHECK(window != None);
    config.rows = 3;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_create(
              display,
              window,
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    CHECK(boxrig_x11_input_adapter_set_key_router(
              adapter,
              capture_key_identity,
              &trace) == 0);

    keycode = XKeysymToKeycode(display, XK_slash);
    CHECK(keycode != 0);
    memset(overridden_keysyms, 0, sizeof(overridden_keysyms));
    overridden_keycode = keycode;
    overridden_keysyms[0][0] = XK_slash;
    overridden_keysyms[0][1] = XK_question;
    overridden_keysyms[1][0] = 0x010003bbu;
    overridden_keysyms[1][1] = 0x0100039bu;
    keymap_overridden = true;

    memset(&event, 0, sizeof(event));
    event.xkey.display = display;
    event.xkey.window = window;
    event.xkey.root = RootWindow(display, DefaultScreen(display));
    event.xkey.same_screen = True;
    event.xkey.keycode = keycode;
    event.xkey.state = XkbBuildCoreState(ShiftMask, 1);
    event.xkey.type = KeyPress;
    event.xkey.time = 1;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    event.xkey.type = KeyRelease;
    event.xkey.time = 2;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(trace.count == 2);
    CHECK(trace.events[0].codepoint == 0x03bb);
    CHECK(trace.events[0].shifted_codepoint == 0x039b);
    CHECK(trace.events[0].base_layout_codepoint == '/');
    CHECK((trace.events[0].modifiers & BOXRIG_MODIFIER_SHIFT) != 0);
    CHECK(trace.events[1].codepoint == 0x03bb);
    CHECK(trace.events[1].shifted_codepoint == 0x039b);
    CHECK(trace.events[1].base_layout_codepoint == '/');

    memset(&trace, 0, sizeof(trace));
    overridden_keysyms[1][0] = XK_minus;
    overridden_keysyms[1][1] = XK_underscore;
    event.xkey.state = XkbBuildCoreState(0, 1);
    event.xkey.type = KeyPress;
    event.xkey.time = 3;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    event.xkey.type = KeyRelease;
    event.xkey.time = 4;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(trace.count == 2);
    CHECK(trace.events[0].codepoint == '-');
    CHECK(trace.events[0].shifted_codepoint == '_');
    CHECK(trace.events[0].base_layout_codepoint == 0);

    keymap_overridden = false;
    boxrig_x11_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return true;
}

static bool test_application_mouse_routing_captures_the_drag_series(void)
{
    Display *display;
    Window window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ui_allocation_input allocation;
    boxrig_ui_layout_result layout;
    boxrig_x11_input_adapter *adapter = NULL;
    routed_mouse_trace trace;
    XEvent event;
    const uint8_t modes[] = "\x1b[?1003;1006h";
    const uint8_t *output = NULL;
    size_t consumed = 0;

    memset(&trace, 0, sizeof(trace));
    display = XOpenDisplay(NULL);
    CHECK(display != NULL);
    window = XCreateSimpleWindow(
        display,
        RootWindow(display, DefaultScreen(display)),
        0,
        0,
        40,
        60,
        0,
        0,
        0);
    CHECK(window != None);
    config.rows = 3;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    memset(&allocation, 0, sizeof(allocation));
    allocation.allocation_width = 40 * BOXRIG_UI_FIXED_ONE;
    allocation.allocation_height = 60 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_width = 10 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_height = 20 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_metrics_revision = 1;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_x11_input_adapter_create(
              display,
              window,
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    CHECK(boxrig_x11_input_adapter_set_mouse_router(
              adapter,
              view,
              route_left_drag,
              &trace) == 0);
    CHECK(boxrig_x11_input_adapter_set_viewport_origin(adapter, 7, 11) == 0);
    CHECK(boxrig_terminal_feed(
              terminal,
              modes,
              sizeof(modes) - 1,
              &consumed) == BOXRIG_STATUS_OK);

    CHECK(boxrig_x11_input_adapter_set_input_enabled(adapter, false) == BOXRIG_UI_OK);
    memset(&event, 0, sizeof(event));
    event.xbutton.display = display;
    event.xbutton.window = window;
    event.xbutton.root = RootWindow(display, DefaultScreen(display));
    event.xbutton.same_screen = True;
    event.xbutton.x = 15;
    event.xbutton.y = 25;
    event.xbutton.time = 100;
    event.xbutton.type = ButtonPress;
    event.xbutton.button = Button1;
    event.xbutton.state = ControlMask;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    event.xmotion.type = MotionNotify;
    event.xmotion.state = Button1Mask | ShiftMask;
    event.xmotion.time = 110;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    event.xbutton.type = ButtonRelease;
    event.xbutton.state = Button1Mask;
    event.xbutton.button = Button1;
    event.xbutton.time = 120;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(trace.count == 3);
    CHECK(trace.events[0].action == BOXRIG_MOUSE_PRESS);
    CHECK(trace.events[0].x == 8);
    CHECK(trace.events[0].y == 14);
    CHECK(trace.events[0].row == 0);
    CHECK(trace.events[0].column == 0);
    CHECK(trace.events[0].click_count == 1);
    CHECK(trace.events[0].modifiers == BOXRIG_MODIFIER_CONTROL);
    CHECK(trace.hits[0].inside_grid && trace.hits[0].row == 0 &&
          trace.hits[0].visual_column == 0);
    CHECK(trace.hits[0].x == 8 * BOXRIG_UI_FIXED_ONE &&
          trace.hits[0].y == 14 * BOXRIG_UI_FIXED_ONE);
    CHECK(trace.hits[0].layout_generation != 0 &&
          trace.hits[0].annotation_generation != 0);
    CHECK(trace.events[1].action == BOXRIG_MOUSE_MOTION);
    CHECK(trace.events[1].modifiers == BOXRIG_MODIFIER_SHIFT);
    CHECK(trace.events[1].click_count == 0);
    CHECK(trace.events[2].action == BOXRIG_MOUSE_RELEASE);
    CHECK(trace.events[2].modifiers == 0u);
    CHECK(trace.events[2].click_count == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    event.xbutton.type = ButtonPress;
    event.xbutton.state = 0;
    event.xbutton.button = Button1;
    event.xbutton.time = 200;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    event.xbutton.type = ButtonRelease;
    event.xbutton.state = Button1Mask;
    event.xbutton.time = 210;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    event.xbutton.type = ButtonPress;
    event.xbutton.state = 0;
    event.xbutton.time = 300;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    event.xbutton.type = ButtonRelease;
    event.xbutton.state = Button1Mask;
    event.xbutton.time = 310;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    event.xbutton.type = ButtonPress;
    event.xbutton.state = 0;
    event.xbutton.time = 1000;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    event.xbutton.type = ButtonRelease;
    event.xbutton.state = Button1Mask;
    event.xbutton.time = 1010;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(trace.count == 6);
    CHECK(trace.events[3].click_count == 2);
    CHECK(trace.events[4].click_count == 3);
    CHECK(trace.events[5].click_count == 1);

    event.xbutton.type = ButtonPress;
    event.xbutton.state = 0;
    CHECK(boxrig_x11_input_adapter_set_input_enabled(adapter, true) == BOXRIG_UI_OK);
    event.xbutton.button = Button3;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) != 0);

    event.xbutton.type = ButtonRelease;
    event.xbutton.state = Button3Mask;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(boxrig_terminal_output_consume(terminal,
        boxrig_terminal_output_peek(terminal, &output)) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_set_mouse_router(adapter, NULL, NULL, NULL) == 0);
    const char pixel_mode[] = "\033[?1016h";
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)pixel_mode,
        sizeof(pixel_mode) - 1, &consumed) == BOXRIG_STATUS_OK);
    /* The viewport origin is (7, 11), independent of clamped cell indices. */
    event.xmotion.type = MotionNotify;
    event.xmotion.state = 0;
    event.xmotion.x = 5;
    event.xmotion.y = 80;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    const char pixels[] = "\033[<35;-1;70M";
    CHECK(boxrig_terminal_output_peek(terminal, &output) == sizeof(pixels) - 1);
    CHECK(memcmp(output, pixels, sizeof(pixels) - 1) == 0);

    boxrig_x11_input_adapter_destroy(adapter);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return true;
}

static bool test_key_focus_and_mouse_events(void)
{
    Display *display;
    Window window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_x11_input_adapter *adapter = NULL;
    XEvent event;
    KeyCode keycode;
    const uint8_t modes[] = "\x1b[=27u\x1b[?1004;1003;1006h";
    const uint8_t expected[] =
        "\x1b[97;;97u"
        "\x1b[97;1:2;97u"
        "\x1b[97;1:3u"
        "\x1b[57442;5u"
        "\x1b[57442;1:3u"
        "\x1b[I\x1b[O"
        "\x1b[<0;4;3M"
        "\x1b[<32;4;3M"
        "\x1b[<0;4;3m"
        "\x1b[<64;2;1M";
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t output_length;

    (void)setlocale(LC_CTYPE, "");
    (void)XSetLocaleModifiers("");
    display = XOpenDisplay(NULL);
    CHECK(display != NULL);
    window = XCreateSimpleWindow(
        display,
        RootWindow(display, DefaultScreen(display)),
        0,
        0,
        40,
        60,
        0,
        0,
        0);
    CHECK(window != None);
    config.rows = 3;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_create(
              display,
              window,
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    CHECK(boxrig_terminal_feed(
              terminal,
              modes,
              sizeof(modes) - 1,
              &consumed) == BOXRIG_STATUS_OK);

    memset(&event, 0, sizeof(event));
    event.xkey.display = display;
    event.xkey.window = window;
    event.xkey.root = RootWindow(display, DefaultScreen(display));
    event.xkey.same_screen = True;
    event.xkey.type = KeyPress;
    event.xkey.time = 1;
    keycode = XKeysymToKeycode(display, XK_a);
    CHECK(keycode != 0);
    event.xkey.keycode = keycode;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    event.xkey.time = 2;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    event.xkey.type = KeyRelease;
    event.xkey.time = 3;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);

    event.xkey.type = KeyPress;
    event.xkey.time = 4;
    event.xkey.state = 0;
    keycode = XKeysymToKeycode(display, XK_Control_L);
    CHECK(keycode != 0);
    event.xkey.keycode = keycode;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    event.xkey.type = KeyRelease;
    event.xkey.time = 5;
    event.xkey.state = ControlMask;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);

    memset(&event, 0, sizeof(event));
    event.xfocus.display = display;
    event.xfocus.window = window;
    event.xfocus.type = FocusIn;
    event.xfocus.mode = NotifyNormal;
    event.xfocus.detail = NotifyNonlinear;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    event.xfocus.type = FocusOut;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);

    CHECK(boxrig_x11_input_adapter_set_cell_size(adapter, 5, 10) == 0);

    memset(&event, 0, sizeof(event));
    event.xbutton.display = display;
    event.xbutton.window = window;
    event.xbutton.root = RootWindow(display, DefaultScreen(display));
    event.xbutton.same_screen = True;
    event.xbutton.type = ButtonPress;
    event.xbutton.button = Button1;
    event.xbutton.x = 15;
    event.xbutton.y = 25;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    memset(&event, 0, sizeof(event));
    event.xmotion.display = display;
    event.xmotion.window = window;
    event.xmotion.root = RootWindow(display, DefaultScreen(display));
    event.xmotion.same_screen = True;
    event.xmotion.type = MotionNotify;
    event.xmotion.state = Button1Mask;
    event.xmotion.x = 25;
    event.xmotion.y = 45;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    memset(&event, 0, sizeof(event));
    event.xbutton.display = display;
    event.xbutton.window = window;
    event.xbutton.root = RootWindow(display, DefaultScreen(display));
    event.xbutton.same_screen = True;
    event.xbutton.type = ButtonRelease;
    event.xbutton.state = Button1Mask;
    event.xbutton.button = Button1;
    event.xbutton.x = 25;
    event.xbutton.y = 45;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    event.xbutton.type = ButtonPress;
    event.xbutton.state = 0;
    event.xbutton.button = Button4;
    event.xbutton.x = 5;
    event.xbutton.y = 5;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    event.xbutton.type = ButtonRelease;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);

    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, output_length) == 0);

    boxrig_x11_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return true;
}

static bool test_pending_key_retries_atomically(void)
{
    Display *display;
    Window window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_x11_input_adapter *adapter = NULL;
    XEvent event;
    const uint8_t mode[] = "\x1b[=11u";
    const uint8_t *output = NULL;
    size_t consumed = 0;

    display = XOpenDisplay(NULL);
    CHECK(display != NULL);
    window = XCreateSimpleWindow(
        display,
        RootWindow(display, DefaultScreen(display)),
        0,
        0,
        10,
        10,
        0,
        0,
        0);
    config.rows = 1;
    config.columns = 1;
    config.output_capacity = 5;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_create(
              display,
              window,
              window,
              terminal,
              10,
              10,
              1024,
              config.output_capacity,
              &adapter) == 0);
    CHECK(boxrig_terminal_feed(
              terminal,
              mode,
              sizeof(mode) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_text(terminal, "x", 1) == BOXRIG_STATUS_OK);

    memset(&event, 0, sizeof(event));
    event.xkey.type = KeyPress;
    event.xkey.display = display;
    event.xkey.window = window;
    event.xkey.root = RootWindow(display, DefaultScreen(display));
    event.xkey.same_screen = True;
    event.xkey.keycode = XKeysymToKeycode(display, XK_Escape);
    CHECK(event.xkey.keycode != 0);
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(boxrig_x11_input_adapter_blocked(adapter));
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(memcmp(output, "x", 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_retry(adapter) == 0);
    CHECK(!boxrig_x11_input_adapter_blocked(adapter));
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 5);
    CHECK(memcmp(output, "\x1b[27u", 5) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 5) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_retry(adapter) == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    boxrig_x11_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return true;
}

static bool test_streamed_paste_normalizes_and_retries(void)
{
    Display *display;
    Window window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_x11_input_adapter *adapter = NULL;
    const uint8_t mode[] = "\x1b[?2004h";
    const uint8_t raw[] = {
        'a', '\r', '\n', 'b', '\r', 'c', '\t', 0x1b, '[', '2', '0', '1',
        '~', 0x01, 0xc2, 0x80, 'z', 0xe6, 0x97, 0xa5, 0xf0, 0x9f, 0x99, 0x82};
    const uint8_t invalid[] = {0xff};
    const uint8_t expected[] =
        "\x1b[200~a\nb\nc\t[201~z日🙂\x1b[201~";
    const uint8_t *output = NULL;
    uint8_t observed[128];
    size_t observed_length = 0;
    size_t consumed = 0;
    size_t attempts;

    display = XOpenDisplay(NULL);
    CHECK(display != NULL);
    window = XCreateSimpleWindow(
        display,
        RootWindow(display, DefaultScreen(display)),
        0,
        0,
        10,
        10,
        0,
        0,
        0);
    config.rows = 1;
    config.columns = 1;
    config.output_capacity = 6;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_create(
              display,
              window,
              window,
              terminal,
              10,
              10,
              1024,
              1,
              &adapter) == 0);
    CHECK(boxrig_terminal_feed(
              terminal,
              mode,
              sizeof(mode) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_queue_paste(
              adapter,
              raw,
              sizeof(raw)) == 0);
    CHECK(boxrig_x11_input_adapter_blocked(adapter));
    for (attempts = 0; attempts < 32; attempts++) {
        size_t length = boxrig_terminal_output_peek(terminal, &output);

        if (length != 0) {
            CHECK(length <= sizeof(observed) - observed_length);
            memcpy(observed + observed_length, output, length);
            observed_length += length;
            CHECK(boxrig_terminal_output_consume(terminal, length) ==
                  BOXRIG_STATUS_OK);
        }
        CHECK(boxrig_x11_input_adapter_retry(adapter) == 0);
        if (!boxrig_x11_input_adapter_blocked(adapter) &&
            boxrig_terminal_output_peek(terminal, &output) == 0) {
            break;
        }
    }
    CHECK(attempts < 32);
    CHECK(observed_length == sizeof(expected) - 1);
    CHECK(memcmp(observed, expected, observed_length) == 0);
    CHECK(boxrig_x11_input_adapter_queue_paste(
              adapter,
              invalid,
              sizeof(invalid)) == -1);
    CHECK(!boxrig_x11_input_adapter_blocked(adapter));

    boxrig_x11_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return true;
}

static bool test_primary_selection_paste(void)
{
    for (unsigned boundary = 0; boundary < 2; boundary++) {
    Display *display;
    Window window;
    Window owner;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_x11_input_adapter *adapter = NULL;
    XEvent event;
    XEvent request_event;
    XEvent notify;
    Atom utf8_string;
    Atom targets;
    Atom timestamp_property;
    Atom offered_targets[1];
    Atom property;
    boxrig_ui_view_action action;
    boxrig_ui_view_action_capabilities capabilities;
    const uint8_t mode[] = "\x1b[?2004h";
    const uint8_t selection[] = "paste\rtext";
    const uint8_t expected[] = "\x1b[200~paste\ntext\x1b[201~";
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t output_length;
    Time paste_time;

    display = XOpenDisplay(NULL);
    CHECK(display != NULL);
    window = XCreateSimpleWindow(
        display,
        RootWindow(display, DefaultScreen(display)),
        0,
        0,
        10,
        10,
        0,
        0,
        0);
    owner = XCreateSimpleWindow(
        display,
        RootWindow(display, DefaultScreen(display)),
        0,
        0,
        1,
        1,
        0,
        0,
        0);
    CHECK(window != None && owner != None);
    XSelectInput(
        display,
        window,
        PropertyChangeMask | StructureNotifyMask);
    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_create(
              display,
              window,
              window,
              terminal,
              10,
              10,
              1024,
              config.output_capacity,
              &adapter) == 0);
    CHECK(boxrig_terminal_feed(
              terminal,
              mode,
              sizeof(mode) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    utf8_string = XInternAtom(display, "UTF8_STRING", False);
    targets = XInternAtom(display, "TARGETS", False);
    timestamp_property =
        XInternAtom(display, "_BOXRIG_TIMESTAMP", False);
    CHECK(utf8_string != None && targets != None &&
          timestamp_property != None);
    offered_targets[0] = utf8_string;
    XSetSelectionOwner(display, XA_PRIMARY, owner, CurrentTime);
    CHECK(XGetSelectionOwner(display, XA_PRIMARY) == owner);

    memset(&event, 0, sizeof(event));
    event.xbutton.type = ButtonPress;
    event.xbutton.display = display;
    event.xbutton.window = window;
    event.xbutton.root = RootWindow(display, DefaultScreen(display));
    event.xbutton.same_screen = True;
    event.xbutton.state = ShiftMask;
    event.xbutton.button = Button2;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    event.xbutton.type = ButtonRelease;
    event.xbutton.state = ShiftMask | Button2Mask;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    XSync(display, False);
    CHECK(!XCheckTypedWindowEvent(
        display,
        owner,
        SelectionRequest,
        &request_event));

    capabilities =
        boxrig_x11_input_adapter_view_action_capabilities(adapter);
    CHECK(capabilities.copy_targets ==
          (BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION |
           BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD));
    CHECK(capabilities.paste_targets ==
          (BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION |
           BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD));
    action.kind = BOXRIG_UI_VIEW_ACTION_PASTE;
    action.clipboard_target =
        BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
    CHECK(boxrig_ui_view_action_supported(&capabilities, &action));
    CHECK(boxrig_x11_input_adapter_apply_view_action(
              adapter,
              NULL,
              &action) ==
          BOXRIG_UI_OK);
    action.kind = BOXRIG_UI_VIEW_ACTION_COPY;
    CHECK(boxrig_ui_view_action_supported(&capabilities, &action));
    CHECK(boxrig_x11_input_adapter_apply_view_action(
              adapter,
              NULL,
              &action) ==
          BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(take_atom_property_event(
        display,
        window,
        timestamp_property,
        PropertyNewValue,
        &event));
    paste_time = event.xproperty.time;
    CHECK(paste_time != CurrentTime);
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    XSync(display, False);
    CHECK(XCheckTypedWindowEvent(
              display,
              owner,
              SelectionRequest,
              &request_event));
    property = request_event.xselectionrequest.property == None
                   ? request_event.xselectionrequest.target
                   : request_event.xselectionrequest.property;
    CHECK(request_event.xselectionrequest.target == targets);
    CHECK(request_event.xselectionrequest.time == paste_time);
    if (boundary != 0) {
        XEvent late = {0};
        late.xselection.type = SelectionNotify;
        late.xselection.display = display;
        late.xselection.requestor = request_event.xselectionrequest.requestor;
        late.xselection.selection = request_event.xselectionrequest.selection;
        late.xselection.target = request_event.xselectionrequest.target;
        late.xselection.property = None;
        late.xselection.time = request_event.xselectionrequest.time;
        CHECK(boxrig_x11_input_adapter_set_input_enabled(adapter, false) == BOXRIG_UI_OK);
        capabilities = boxrig_x11_input_adapter_view_action_capabilities(adapter);
        CHECK(capabilities.copy_targets != 0 && capabilities.paste_targets == 0);
        CHECK(boxrig_x11_input_adapter_set_input_enabled(adapter, true) == BOXRIG_UI_OK);
        action.kind = BOXRIG_UI_VIEW_ACTION_PASTE;
        CHECK(boxrig_x11_input_adapter_apply_view_action(adapter, NULL, &action) == BOXRIG_UI_OK);
        CHECK(take_atom_property_event(display, window, timestamp_property, PropertyNewValue, &event));
        CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
        XSync(display, False);
        CHECK(XCheckTypedWindowEvent(display, owner, SelectionRequest, &request_event));
        /* An old owner reply must not cancel the new incoming request. */
        CHECK(boxrig_x11_input_adapter_process_event(adapter, &late) == 0);
        property = request_event.xselectionrequest.property;
    }
    XChangeProperty(
        display,
        request_event.xselectionrequest.requestor,
        property,
        XA_ATOM,
        32,
        PropModeReplace,
        (const unsigned char *)offered_targets,
        1);
    memset(&notify, 0, sizeof(notify));
    notify.xselection.type = SelectionNotify;
    notify.xselection.display = display;
    notify.xselection.requestor = request_event.xselectionrequest.requestor;
    notify.xselection.selection = request_event.xselectionrequest.selection;
    notify.xselection.target = request_event.xselectionrequest.target;
    notify.xselection.property = property;
    notify.xselection.time = request_event.xselectionrequest.time;
    CHECK(XSendEvent(
              display,
              notify.xselection.requestor,
              False,
              0,
              &notify) != 0);
    XSync(display, False);
    CHECK(XCheckTypedWindowEvent(
              display,
              notify.xselection.requestor,
              SelectionNotify,
              &event));
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    XSync(display, False);
    CHECK(XCheckTypedWindowEvent(
              display,
              owner,
              SelectionRequest,
              &request_event));
    property = request_event.xselectionrequest.property == None
                   ? request_event.xselectionrequest.target
                   : request_event.xselectionrequest.property;
    CHECK(request_event.xselectionrequest.target == utf8_string);
    XChangeProperty(
        display,
        request_event.xselectionrequest.requestor,
        property,
        utf8_string,
        8,
        PropModeReplace,
        selection,
        (int)(sizeof(selection) - 1));
    notify.xselection.target = request_event.xselectionrequest.target;
    notify.xselection.property = property;
    notify.xselection.time = request_event.xselectionrequest.time;
    CHECK(XSendEvent(
              display,
              notify.xselection.requestor,
              False,
              0,
              &notify) != 0);
    XSync(display, False);
    CHECK(XCheckTypedWindowEvent(
              display,
              notify.xselection.requestor,
              SelectionNotify,
              &event));
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, output_length) == 0);

    boxrig_x11_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, owner);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    }
    return true;
}

static bool test_copy_ownership_targets_direct_incr_and_limits(void)
{
    static const uint8_t copied_text[] = "copy payload";
    Display *owner_display;
    Display *requestor_display;
    Window owner_window;
    Window requestors[3];
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_x11_input_adapter *adapter = NULL;
    boxrig_ui_hit_test_result hit;
    boxrig_ui_view_action action;
    boxrig_ui_view_action_capabilities capabilities;
    XEvent event;
    XEvent notify;
    Atom clipboard;
    Atom targets;
    Atom utf8_string;
    Atom incr;
    Atom timestamp;
    Atom timestamp_property;
    Atom properties[3];
    Atom actual_type;
    int actual_format;
    unsigned long item_count;
    unsigned long bytes_after;
    unsigned char *value = NULL;
    uint8_t received[sizeof(copied_text)];
    size_t received_length = 0;
    size_t consumed = 0;
    size_t index;
    Time ownership_time;
    bool found_targets = false;
    bool found_utf8 = false;
    bool found_string = false;
    bool found_timestamp = false;

    owner_display = XOpenDisplay(NULL);
    requestor_display = XOpenDisplay(NULL);
    CHECK(owner_display != NULL && requestor_display != NULL);
    owner_window = XCreateSimpleWindow(
        owner_display,
        RootWindow(owner_display, DefaultScreen(owner_display)),
        0,
        0,
        32,
        32,
        0,
        0,
        0);
    CHECK(owner_window != None);
    XSelectInput(
        owner_display,
        owner_window,
        PropertyChangeMask | StructureNotifyMask);
    for (index = 0; index < 3; index++) {
        requestors[index] = XCreateSimpleWindow(
            requestor_display,
            RootWindow(
                requestor_display,
                DefaultScreen(requestor_display)),
            0,
            0,
            8,
            8,
            0,
            0,
            0);
        CHECK(requestors[index] != None);
        XSelectInput(
            requestor_display,
            requestors[index],
            PropertyChangeMask | StructureNotifyMask);
    }
    config.rows = 1;
    config.columns = 32;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              copied_text,
              sizeof(copied_text) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    memset(&hit, 0, sizeof(hit));
    hit.inside_grid = true;
    hit.text_position_valid = true;
    CHECK(boxrig_terminal_get_text_position(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              0,
              0,
              &hit.leading_position) == BOXRIG_STATUS_OK);
    hit.trailing_position = hit.leading_position;
    hit.position = hit.leading_position;
    CHECK(boxrig_ui_view_begin_selection(
              view,
              BOXRIG_UI_SELECTION_LINE,
              &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_input_adapter_create(
              owner_display,
              owner_window,
              owner_window,
              terminal,
              8,
              16,
              1024,
              config.output_capacity,
              &adapter) == 0);
    CHECK(boxrig_x11_input_adapter_set_input_enabled(adapter, false) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_input_adapter_set_clipboard_transfer_limits(
              adapter,
              64,
              2,
              2 * (sizeof(copied_text) - 1)) == 0);
    capabilities =
        boxrig_x11_input_adapter_view_action_capabilities(adapter);
    CHECK(capabilities.copy_targets ==
          (BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION |
           BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD));
    action.kind = BOXRIG_UI_VIEW_ACTION_COPY;
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    CHECK(boxrig_x11_input_adapter_apply_view_action(
              adapter,
              view,
              &action) == BOXRIG_UI_OK);
    timestamp_property =
        XInternAtom(owner_display, "_BOXRIG_TIMESTAMP", False);
    CHECK(timestamp_property != None);
    CHECK(take_atom_property_event(
        owner_display,
        owner_window,
        timestamp_property,
        PropertyNewValue,
        &event));
    ownership_time = event.xproperty.time;
    CHECK(ownership_time != CurrentTime);
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    XSync(owner_display, False);

    clipboard = XInternAtom(requestor_display, "CLIPBOARD", False);
    targets = XInternAtom(requestor_display, "TARGETS", False);
    utf8_string = XInternAtom(requestor_display, "UTF8_STRING", False);
    incr = XInternAtom(requestor_display, "INCR", False);
    timestamp = XInternAtom(requestor_display, "TIMESTAMP", False);
    CHECK(clipboard != None && targets != None && utf8_string != None &&
          incr != None && timestamp != None);
    CHECK(XGetSelectionOwner(requestor_display, clipboard) == owner_window);
    for (index = 0; index < 3; index++) {
        char property_name[64];

        CHECK(snprintf(
                  property_name,
                  sizeof(property_name),
                  "_BOXRIG_COPY_%lu",
                  (unsigned long)index) > 0);
        properties[index] =
            XInternAtom(requestor_display, property_name, False);
        CHECK(properties[index] != None);
    }

    XConvertSelection(
        requestor_display,
        clipboard,
        targets,
        properties[0],
        requestors[0],
        CurrentTime);
    XSync(requestor_display, False);
    CHECK(take_window_event(
        owner_display,
        owner_window,
        SelectionRequest,
        &event));
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    XSync(owner_display, False);
    CHECK(take_window_event(
        requestor_display,
        requestors[0],
        SelectionNotify,
        &notify));
    CHECK(notify.xselection.property == properties[0]);
    CHECK(XGetWindowProperty(
              requestor_display,
              requestors[0],
              properties[0],
              0,
              16,
              True,
              XA_ATOM,
              &actual_type,
              &actual_format,
              &item_count,
              &bytes_after,
              &value) == Success);
    CHECK(actual_type == XA_ATOM && actual_format == 32 &&
          bytes_after == 0);
    for (index = 0; index < item_count; index++) {
        Atom offered = ((Atom *)value)[index];

        found_targets |= offered == targets;
        found_utf8 |= offered == utf8_string;
        found_string |= offered == XA_STRING;
        found_timestamp |= offered == timestamp;
    }
    XFree(value);
    value = NULL;
    CHECK(found_targets && found_utf8 && found_string && found_timestamp);

    XConvertSelection(
        requestor_display,
        clipboard,
        timestamp,
        properties[0],
        requestors[0],
        ownership_time);
    XSync(requestor_display, False);
    CHECK(take_window_event(
        owner_display,
        owner_window,
        SelectionRequest,
        &event));
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    XSync(owner_display, False);
    CHECK(take_window_event(
        requestor_display,
        requestors[0],
        SelectionNotify,
        &notify));
    CHECK(notify.xselection.property == properties[0]);
    CHECK(XGetWindowProperty(
              requestor_display,
              requestors[0],
              properties[0],
              0,
              1,
              True,
              XA_INTEGER,
              &actual_type,
              &actual_format,
              &item_count,
              &bytes_after,
              &value) == Success);
    CHECK(actual_type == XA_INTEGER && actual_format == 32 &&
          item_count == 1 && bytes_after == 0 &&
          ((const unsigned long *)value)[0] == ownership_time);
    XFree(value);
    value = NULL;

    XConvertSelection(
        requestor_display,
        clipboard,
        utf8_string,
        properties[0],
        requestors[0],
        CurrentTime);
    XSync(requestor_display, False);
    CHECK(take_window_event(
        owner_display,
        owner_window,
        SelectionRequest,
        &event));
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    XSync(owner_display, False);
    CHECK(take_window_event(
        requestor_display,
        requestors[0],
        SelectionNotify,
        &notify));
    CHECK(XGetWindowProperty(
              requestor_display,
              requestors[0],
              properties[0],
              0,
              64,
              True,
              AnyPropertyType,
              &actual_type,
              &actual_format,
              &item_count,
              &bytes_after,
              &value) == Success);
    CHECK(actual_type == utf8_string && actual_format == 8 &&
          item_count == sizeof(copied_text) - 1 && bytes_after == 0);
    CHECK(memcmp(value, copied_text, item_count) == 0);
    XFree(value);
    value = NULL;

    CHECK(boxrig_x11_input_adapter_set_clipboard_transfer_limits(
              adapter,
              4,
              2,
              sizeof(copied_text) - 2) == 0);
    XConvertSelection(
        requestor_display,
        clipboard,
        utf8_string,
        properties[0],
        requestors[0],
        CurrentTime);
    XSync(requestor_display, False);
    CHECK(take_window_event(
        owner_display,
        owner_window,
        SelectionRequest,
        &event));
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    XSync(owner_display, False);
    CHECK(take_window_event(
        requestor_display,
        requestors[0],
        SelectionNotify,
        &notify));
    CHECK(notify.xselection.property == None);

    maximum_request_size_overridden = true;
    maximum_request_size_override = 9;
    extended_maximum_request_size_override = 0;
    CHECK(boxrig_x11_input_adapter_set_clipboard_transfer_limits(
              adapter,
              64,
              2,
              3 * (sizeof(copied_text) - 1)) == 0);
    maximum_request_size_overridden = false;
    XConvertSelection(
        requestor_display,
        clipboard,
        utf8_string,
        properties[0],
        requestors[0],
        CurrentTime);
    XSync(requestor_display, False);
    CHECK(take_window_event(
        owner_display,
        owner_window,
        SelectionRequest,
        &event));
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    XSync(owner_display, False);
    CHECK(take_window_event(
        requestor_display,
        requestors[0],
        SelectionNotify,
        &notify));
    CHECK(XGetWindowProperty(
              requestor_display,
              requestors[0],
              properties[0],
              0,
              1,
              True,
              AnyPropertyType,
              &actual_type,
              &actual_format,
              &item_count,
              &bytes_after,
              &value) == Success);
    CHECK(actual_type == incr && actual_format == 32 && item_count == 1);
    XFree(value);
    value = NULL;
    while (true) {
        CHECK(take_property_event(
            owner_display,
            requestors[0],
            PropertyDelete,
            &event));
        CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
        XSync(owner_display, False);
        CHECK(take_property_event(
            requestor_display,
            requestors[0],
            PropertyNewValue,
            &event));
        CHECK(XGetWindowProperty(
                  requestor_display,
                  requestors[0],
                  properties[0],
                  0,
                  64,
                  True,
                  AnyPropertyType,
                  &actual_type,
                  &actual_format,
                  &item_count,
                  &bytes_after,
                  &value) == Success);
        CHECK(actual_type == utf8_string && actual_format == 8 &&
              bytes_after == 0);
        if (item_count == 0) {
            XFree(value);
            value = NULL;
            break;
        }
        CHECK(received_length + item_count <= sizeof(received));
        memcpy(received + received_length, value, item_count);
        received_length += item_count;
        XFree(value);
        value = NULL;
    }
    CHECK(received_length == sizeof(copied_text) - 1);
    CHECK(memcmp(received, copied_text, received_length) == 0);

    for (index = 0; index < 2; index++) {
        XConvertSelection(
            requestor_display,
            clipboard,
            utf8_string,
            properties[index],
            requestors[index],
            CurrentTime);
        XSync(requestor_display, False);
        CHECK(take_window_event(
            owner_display,
            owner_window,
            SelectionRequest,
            &event));
        CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
        XSync(owner_display, False);
        CHECK(take_window_event(
            requestor_display,
            requestors[index],
            SelectionNotify,
            &notify));
        CHECK(notify.xselection.property == properties[index]);
        CHECK(XGetWindowProperty(
                  requestor_display,
                  requestors[index],
                  properties[index],
                  0,
                  1,
                  True,
                  AnyPropertyType,
                  &actual_type,
                  &actual_format,
                  &item_count,
                  &bytes_after,
                  &value) == Success);
        CHECK(actual_type == incr);
        XFree(value);
        value = NULL;
    }
    XConvertSelection(
        requestor_display,
        clipboard,
        utf8_string,
        properties[2],
        requestors[2],
        CurrentTime);
    XSync(requestor_display, False);
    CHECK(take_window_event(
        owner_display,
        owner_window,
        SelectionRequest,
        &event));
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    XSync(owner_display, False);
    CHECK(take_window_event(
        requestor_display,
        requestors[2],
        SelectionNotify,
        &notify));
    CHECK(notify.xselection.property == None);

    for (index = 0; index < 2; index++) {
        XDestroyWindow(requestor_display, requestors[index]);
        XSync(requestor_display, False);
        CHECK(take_window_event(
            owner_display,
            requestors[index],
            DestroyNotify,
            &event));
        CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
        requestors[index] = None;
    }
    CHECK(boxrig_x11_input_adapter_set_clipboard_transfer_limits(
              adapter,
              4,
              3,
              2 * (sizeof(copied_text) - 1)) == 0);
    for (index = 0; index < 2; index++) {
        XConvertSelection(
            requestor_display,
            clipboard,
            utf8_string,
            properties[index],
            requestors[2],
            CurrentTime);
        XSync(requestor_display, False);
        CHECK(take_window_event(
            owner_display,
            owner_window,
            SelectionRequest,
            &event));
        CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
        XSync(owner_display, False);
        CHECK(take_window_event(
            requestor_display,
            requestors[2],
            SelectionNotify,
            &notify));
        CHECK(notify.xselection.property == properties[index]);
        CHECK(XGetWindowProperty(
                  requestor_display,
                  requestors[2],
                  properties[index],
                  0,
                  1,
                  True,
                  AnyPropertyType,
                  &actual_type,
                  &actual_format,
                  &item_count,
                  &bytes_after,
                  &value) == Success);
        CHECK(actual_type == incr);
        XFree(value);
        value = NULL;
    }
    XConvertSelection(
        requestor_display,
        clipboard,
        utf8_string,
        properties[2],
        requestors[2],
        CurrentTime);
    XSync(requestor_display, False);
    CHECK(take_window_event(
        owner_display,
        owner_window,
        SelectionRequest,
        &event));
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    XSync(owner_display, False);
    CHECK(take_window_event(
        requestor_display,
        requestors[2],
        SelectionNotify,
        &notify));
    CHECK(notify.xselection.property == None);

    XSetSelectionOwner(
        requestor_display,
        clipboard,
        requestors[2],
        CurrentTime);
    XSync(requestor_display, False);
    CHECK(take_window_event(
        owner_display,
        owner_window,
        SelectionClear,
        &event));
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(XGetSelectionOwner(owner_display, clipboard) == requestors[2]);
    CHECK(boxrig_x11_input_adapter_apply_view_action(
              adapter,
              view,
              &action) == BOXRIG_UI_OK);
    CHECK(take_atom_property_event(
        owner_display,
        owner_window,
        timestamp_property,
        PropertyNewValue,
        &event));
    CHECK(event.xproperty.time != CurrentTime);
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    XSync(owner_display, False);
    CHECK(XGetSelectionOwner(owner_display, clipboard) == owner_window);

    boxrig_x11_input_adapter_destroy(adapter);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    for (index = 0; index < 3; index++) {
        if (requestors[index] != None) {
            XDestroyWindow(requestor_display, requestors[index]);
        }
    }
    XDestroyWindow(owner_display, owner_window);
    XCloseDisplay(requestor_display);
    XCloseDisplay(owner_display);
    return true;
}

static bool test_incr_paste_owner_loss_and_capacity(void)
{
    static const uint8_t chunks[][7] = {"paste\r", "\nchunk"};
    static const uint8_t expected[] = "paste\nchunk";
    static const uint8_t competing_paste[] = "competing";
    Display *input_display;
    Display *source_display;
    Window input_window;
    Window source_window;
    Window replacement_owner;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_x11_input_adapter *adapter = NULL;
    boxrig_ui_view_action action;
    XEvent request;
    XEvent notify;
    XEvent event;
    Atom primary;
    Atom targets;
    Atom utf8_string;
    Atom incr;
    Atom timestamp_property;
    Atom property;
    Atom offered_targets[1];
    unsigned long announced_length;
    const uint8_t *output = NULL;
    size_t output_length;
    size_t index;
    Time paste_time;

    input_display = XOpenDisplay(NULL);
    source_display = XOpenDisplay(NULL);
    CHECK(input_display != NULL && source_display != NULL);
    input_window = XCreateSimpleWindow(
        input_display,
        RootWindow(input_display, DefaultScreen(input_display)),
        0,
        0,
        16,
        16,
        0,
        0,
        0);
    source_window = XCreateSimpleWindow(
        source_display,
        RootWindow(source_display, DefaultScreen(source_display)),
        0,
        0,
        16,
        16,
        0,
        0,
        0);
    replacement_owner = XCreateSimpleWindow(
        source_display,
        RootWindow(source_display, DefaultScreen(source_display)),
        0,
        0,
        16,
        16,
        0,
        0,
        0);
    CHECK(input_window != None && source_window != None &&
          replacement_owner != None);
    XSelectInput(
        input_display,
        input_window,
        PropertyChangeMask | StructureNotifyMask);
    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_create(
              input_display,
              input_window,
              input_window,
              terminal,
              8,
              16,
              32,
              config.output_capacity,
              &adapter) == 0);
    primary = XA_PRIMARY;
    targets = XInternAtom(source_display, "TARGETS", False);
    utf8_string = XInternAtom(source_display, "UTF8_STRING", False);
    incr = XInternAtom(source_display, "INCR", False);
    timestamp_property =
        XInternAtom(input_display, "_BOXRIG_TIMESTAMP", False);
    CHECK(targets != None && utf8_string != None && incr != None &&
          timestamp_property != None);
    offered_targets[0] = utf8_string;
    XSetSelectionOwner(
        source_display,
        primary,
        source_window,
        CurrentTime);
    XSync(source_display, False);
    CHECK(XGetSelectionOwner(input_display, primary) == source_window);

    action.kind = BOXRIG_UI_VIEW_ACTION_PASTE;
    action.clipboard_target =
        BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
    CHECK(boxrig_x11_input_adapter_apply_view_action(
              adapter,
              NULL,
              &action) == BOXRIG_UI_OK);
    XSync(input_display, False);
    CHECK(boxrig_x11_input_adapter_queue_paste(
              adapter,
              competing_paste,
              sizeof(competing_paste) - 1) == -1);
    CHECK(take_atom_property_event(
        input_display,
        input_window,
        timestamp_property,
        PropertyNewValue,
        &event));
    paste_time = event.xproperty.time;
    CHECK(paste_time != CurrentTime);
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    XSync(input_display, False);
    CHECK(take_window_event(
        source_display,
        source_window,
        SelectionRequest,
        &request));
    CHECK(request.xselectionrequest.target == targets);
    CHECK(request.xselectionrequest.time == paste_time);
    property = request.xselectionrequest.property;
    XSelectInput(
        source_display,
        request.xselectionrequest.requestor,
        PropertyChangeMask | StructureNotifyMask);
    XChangeProperty(
        source_display,
        request.xselectionrequest.requestor,
        property,
        XA_ATOM,
        32,
        PropModeReplace,
        (const unsigned char *)offered_targets,
        1);
    memset(&notify, 0, sizeof(notify));
    notify.xselection.type = SelectionNotify;
    notify.xselection.display = source_display;
    notify.xselection.requestor = request.xselectionrequest.requestor;
    notify.xselection.selection = request.xselectionrequest.selection;
    notify.xselection.requestor = request.xselectionrequest.requestor;
    notify.xselection.target = request.xselectionrequest.target;
    notify.xselection.property = property;
    notify.xselection.time = request.xselectionrequest.time;
    CHECK(XSendEvent(
              source_display,
              notify.xselection.requestor,
              False,
              0,
              &notify) != 0);
    XSync(source_display, False);
    CHECK(take_window_event(
        input_display,
        request.xselectionrequest.requestor,
        SelectionNotify,
        &event));
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    XSync(input_display, False);
    CHECK(take_window_event(
        source_display,
        source_window,
        SelectionRequest,
        &request));
    CHECK(request.xselectionrequest.target == utf8_string);

    announced_length = sizeof(chunks) - 2;
    XChangeProperty(
        source_display,
        request.xselectionrequest.requestor,
        request.xselectionrequest.property,
        incr,
        32,
        PropModeReplace,
        (const unsigned char *)&announced_length,
        1);
    notify.xselection.requestor = request.xselectionrequest.requestor;
    notify.xselection.target = request.xselectionrequest.target;
    notify.xselection.property = request.xselectionrequest.property;
    notify.xselection.time = request.xselectionrequest.time;
    CHECK(XSendEvent(
              source_display,
              notify.xselection.requestor,
              False,
              0,
              &notify) != 0);
    XSync(source_display, False);
    CHECK(take_window_event(
        input_display,
        request.xselectionrequest.requestor,
        SelectionNotify,
        &event));
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(take_property_event(
        source_display,
        request.xselectionrequest.requestor,
        PropertyDelete,
        &event));

    XSetSelectionOwner(
        source_display,
        primary,
        replacement_owner,
        CurrentTime);
    XSync(source_display, False);
    CHECK(XGetSelectionOwner(input_display, primary) == replacement_owner);

    for (index = 0; index < sizeof(chunks) / sizeof(chunks[0]); index++) {
        XChangeProperty(
            source_display,
            request.xselectionrequest.requestor,
            property,
            utf8_string,
            8,
            PropModeReplace,
            chunks[index],
            (int)(sizeof(chunks[index]) - 1));
        XSync(source_display, False);
        CHECK(take_property_event(
            input_display,
            request.xselectionrequest.requestor,
            PropertyNewValue,
            &event));
        CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
        CHECK(take_property_event(
            source_display,
            request.xselectionrequest.requestor,
            PropertyDelete,
            &event));
    }
    XChangeProperty(
        source_display,
        request.xselectionrequest.requestor,
        property,
        utf8_string,
        8,
        PropModeReplace,
        NULL,
        0);
    XSync(source_display, False);
    CHECK(take_property_event(
        input_display,
        request.xselectionrequest.requestor,
        PropertyNewValue,
        &event));
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    XSetSelectionOwner(
        source_display,
        primary,
        source_window,
        CurrentTime);
    XSync(source_display, False);
    CHECK(XGetSelectionOwner(input_display, primary) == source_window);

    CHECK(boxrig_x11_input_adapter_apply_view_action(
              adapter,
              NULL,
              &action) == BOXRIG_UI_OK);
    CHECK(take_atom_property_event(
        input_display,
        input_window,
        timestamp_property,
        PropertyNewValue,
        &event));
    CHECK(event.xproperty.time != CurrentTime);
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    XSync(input_display, False);
    CHECK(take_window_event(
        source_display,
        source_window,
        SelectionRequest,
        &request));
    property = request.xselectionrequest.property;
    XChangeProperty(
        source_display,
        request.xselectionrequest.requestor,
        property,
        XA_ATOM,
        32,
        PropModeReplace,
        (const unsigned char *)offered_targets,
        1);
    notify.xselection.requestor = request.xselectionrequest.requestor;
    notify.xselection.target = request.xselectionrequest.target;
    notify.xselection.property = property;
    notify.xselection.time = request.xselectionrequest.time;
    CHECK(XSendEvent(
              source_display,
              request.xselectionrequest.requestor,
              False,
              0,
              &notify) != 0);
    XSync(source_display, False);
    CHECK(take_window_event(
        input_display,
        request.xselectionrequest.requestor,
        SelectionNotify,
        &event));
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    XSync(input_display, False);
    CHECK(take_window_event(
        source_display,
        source_window,
        SelectionRequest,
        &request));
    announced_length = 33;
    XChangeProperty(
        source_display,
        request.xselectionrequest.requestor,
        request.xselectionrequest.property,
        incr,
        32,
        PropModeReplace,
        (const unsigned char *)&announced_length,
        1);
    notify.xselection.requestor = request.xselectionrequest.requestor;
    notify.xselection.target = request.xselectionrequest.target;
    notify.xselection.property = request.xselectionrequest.property;
    notify.xselection.time = request.xselectionrequest.time;
    CHECK(XSendEvent(
              source_display,
              request.xselectionrequest.requestor,
              False,
              0,
              &notify) != 0);
    XSync(source_display, False);
    CHECK(take_window_event(
        input_display,
        request.xselectionrequest.requestor,
        SelectionNotify,
        &event));
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    CHECK(boxrig_x11_input_adapter_apply_view_action(
              adapter,
              NULL,
              &action) == BOXRIG_UI_OK);
    CHECK(take_atom_property_event(
        input_display,
        input_window,
        timestamp_property,
        PropertyNewValue,
        &event));
    CHECK(event.xproperty.time != CurrentTime);
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    XSync(input_display, False);
    CHECK(take_window_event(
        source_display,
        source_window,
        SelectionRequest,
        &request));
    XDestroyWindow(source_display, source_window);
    XSync(source_display, False);
    source_window = None;
    CHECK(take_window_event(
        input_display,
        request.xselectionrequest.owner,
        DestroyNotify,
        &event));
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    boxrig_x11_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(input_display, input_window);
    if (source_window != None) {
        XDestroyWindow(source_display, source_window);
    }
    XDestroyWindow(source_display, replacement_owner);
    XCloseDisplay(source_display);
    XCloseDisplay(input_display);
    return true;
}

static bool test_xim_callbacks_follow_the_common_composition_fixture(void)
{
    Display *display;
    Window window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_x11_input_adapter *adapter = NULL;
    boxrig_composition *composition;
    boxrig_composition_view view;
    wchar_t characters[4];
    XIMFeedback feedback[4];
    XIMText text;
    XIMPreeditDrawCallbackStruct draw;
    boxrig_text_cluster_input layout_clusters[8];
    boxrig_text_layout *layout = NULL;
    XEvent focus_event;
    const uint8_t *output = NULL;
    size_t consumed;
    size_t output_length;
    size_t index;

    (void)setlocale(LC_CTYPE, "");
    CHECK(XSetLocaleModifiers("@im=none") != NULL);
    display = XOpenDisplay(NULL);
    CHECK(display != NULL);
    window = XCreateSimpleWindow(
        display,
        RootWindow(display, DefaultScreen(display)),
        0,
        0,
        80,
        60,
        0,
        0,
        0);
    CHECK(window != None);
    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_create(
              display,
              window,
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    composition = boxrig_x11_input_adapter_composition(adapter);
    CHECK(composition != NULL);
    for (index = 0; index < boxrig_composition_fixture.character_count;
         index++) {
        characters[index] =
            (wchar_t)boxrig_composition_fixture.codepoints[index];
        if (boxrig_composition_fixture.attributes[index] ==
            BOXRIG_COMPOSITION_FIXTURE_INPUT) {
            feedback[index] = XIMUnderline;
        } else if (boxrig_composition_fixture.attributes[index] ==
                   BOXRIG_COMPOSITION_FIXTURE_CONVERTED) {
            feedback[index] = XIMPrimary;
        } else {
            feedback[index] = XIMReverse;
        }
    }
    memset(&text, 0, sizeof(text));
    text.length = (unsigned short)boxrig_composition_fixture.character_count;
    text.feedback = feedback;
    text.encoding_is_wchar = True;
    text.string.wide_char = characters;
    memset(&draw, 0, sizeof(draw));
    draw.caret = 2;
    draw.text = &text;

    CHECK(boxrig_x11_input_adapter_preedit_start(adapter) == 0);
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active);
    CHECK(view.text_length == boxrig_composition_fixture.utf8_length);
    CHECK(memcmp(
              view.text,
              boxrig_composition_fixture.utf8,
              view.text_length) == 0);
    CHECK(view.segment_count == boxrig_composition_fixture.segment_count);
    CHECK(memcmp(
              view.segments,
              boxrig_composition_fixture.segments,
              view.segment_count * sizeof(*view.segments)) == 0);
    CHECK(view.selection_offset ==
          boxrig_composition_fixture.selection_offset);
    CHECK(view.selection_length ==
          boxrig_composition_fixture.selection_length);
    CHECK(view.caret_offset == boxrig_composition_fixture.caret_offset);
    CHECK(view.caret_visible);
    memset(layout_clusters, 0, sizeof(layout_clusters));
    for (index = 0; index < sizeof(layout_clusters) /
                                sizeof(layout_clusters[0]);
         index++) {
        layout_clusters[index].logical_column = index;
        layout_clusters[index].column_count = 1;
        layout_clusters[index].text_offset = index;
        layout_clusters[index].text_length = 1;
    }
    CHECK(boxrig_text_layout_create(
              layout_clusters,
              sizeof(layout_clusters) / sizeof(layout_clusters[0]),
              sizeof(layout_clusters) / sizeof(layout_clusters[0]),
              BOXRIG_TEXT_DIRECTION_LTR,
              &layout) == BOXRIG_TEXT_LAYOUT_OK);
    CHECK(boxrig_composition_synchronize(composition, layout, 10, 20, 15) ==
          BOXRIG_COMPOSITION_OK);
    candidate_spot_set_calls = 0;
    CHECK(boxrig_x11_input_adapter_synchronize_candidate(adapter) == 0);
    CHECK(candidate_spot_set_calls == 1);
    for (index = 0; index < 32; index++) {
        CHECK(boxrig_x11_input_adapter_synchronize_candidate(adapter) == 0);
    }
    CHECK(candidate_spot_set_calls == 1);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.candidate.visible);
    CHECK(view.candidate.column == 3);
    CHECK(view.candidate.spot_x == 30);
    CHECK(view.candidate.spot_y == 15);
    CHECK(boxrig_composition_synchronize(composition, layout, 11, 20, 15) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_x11_input_adapter_synchronize_candidate(adapter) == 0);
    CHECK(candidate_spot_set_calls == 2);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.candidate.spot_x == 33);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\033[2;8H",
              sizeof("\033[2;8H") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof("\033[2;8H") - 1);
    CHECK(boxrig_composition_synchronize(composition, layout, 11, 20, 15) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.candidate.visible);
    CHECK(view.candidate.row == 2 && view.candidate.column == 2);
    CHECK(view.candidate.spot_x == 22 && view.candidate.spot_y == 55);
    CHECK(view.candidate.exclude_x == 22 &&
          view.candidate.exclude_y == 40);
    CHECK(view.candidate.exclude_width == 11 &&
          view.candidate.exclude_height == 20);
    CHECK(boxrig_x11_input_adapter_synchronize_candidate(adapter) == 0);
    CHECK(candidate_spot_set_calls == 3);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\033[3;8H",
              sizeof("\033[3;8H") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof("\033[3;8H") - 1);
    CHECK(boxrig_composition_synchronize(composition, layout, 11, 20, 15) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.candidate.visible);
    CHECK(boxrig_x11_input_adapter_synchronize_candidate(adapter) == 0);
    CHECK(candidate_spot_set_calls == 3);
    boxrig_text_layout_destroy(layout);
    layout = NULL;

    CHECK(boxrig_x11_input_adapter_preedit_done(adapter) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active);
    CHECK(view.last_event == BOXRIG_COMPOSITION_PREEDIT_COMPLETE);
    CHECK(boxrig_x11_input_adapter_preedit_start(adapter) == 0);
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    memset(&focus_event, 0, sizeof(focus_event));
    focus_event.xfocus.display = display;
    focus_event.xfocus.window = window;
    focus_event.xfocus.type = FocusOut;
    focus_event.xfocus.mode = NotifyNormal;
    focus_event.xfocus.detail = NotifyNonlinear;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &focus_event) == 1);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active);
    CHECK(view.last_event == BOXRIG_COMPOSITION_CANCEL);
    CHECK(!view.focused);
    focus_event.xfocus.type = FocusIn;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &focus_event) == 1);

    CHECK(boxrig_x11_input_adapter_preedit_start(adapter) == 0);
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    CHECK(boxrig_x11_input_adapter_commit_utf8(
              adapter,
              boxrig_composition_fixture.commit_utf8,
              boxrig_composition_fixture.commit_utf8_length) == 0);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == boxrig_composition_fixture.commit_utf8_length);
    CHECK(memcmp(
              output,
              boxrig_composition_fixture.commit_utf8,
              output_length) == 0);
    CHECK(boxrig_x11_input_adapter_retry(adapter) == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == output_length);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active && !view.commit_pending);
    CHECK(view.last_event == BOXRIG_COMPOSITION_COMMIT);

    draw.chg_length =
        (int)boxrig_composition_fixture.character_count;
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active);
    CHECK(view.text_length == boxrig_composition_fixture.utf8_length);
    CHECK(memcmp(
              view.text,
              boxrig_composition_fixture.utf8,
              view.text_length) == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == output_length);

    boxrig_x11_input_adapter_destroy(adapter);
    boxrig_text_layout_destroy(layout);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    (void)XSetLocaleModifiers("");
    return true;
}

static bool test_xim_clause_feedback_partial_edits_and_caret_moves(void)
{
    static const char expected_after_insert[] =
        "\343\201\213\343\201\252\350\252\236\346\274\242\345\255\227";
    static const char expected_after_delete[] =
        "\343\201\213\343\201\252\350\252\236\345\255\227";
    Display *display;
    Window window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_x11_input_adapter *adapter = NULL;
    boxrig_composition *composition;
    boxrig_composition_view view;
    wchar_t initial_characters[] = {0x304b, 0x306a, 0x6f22, 0x5b57};
    wchar_t inserted_character[] = {0x8a9e};
    XIMFeedback initial_feedback[] = {
        XIMUnderline,
        XIMUnderline,
        XIMUnderline,
        XIMUnderline,
    };
    XIMFeedback target_feedback[] = {XIMReverse, XIMReverse};
    XIMFeedback converted_feedback[] = {XIMPrimary, XIMPrimary};
    XIMFeedback final_target_feedback[] = {XIMReverse};
    XIMText text;
    XIMText feedback_update;
    XIMPreeditDrawCallbackStruct draw;
    XIMPreeditCaretCallbackStruct caret;

    (void)setlocale(LC_CTYPE, "");
    CHECK(XSetLocaleModifiers("@im=none") != NULL);
    display = XOpenDisplay(NULL);
    CHECK(display != NULL);
    window = XCreateSimpleWindow(
        display,
        RootWindow(display, DefaultScreen(display)),
        0,
        0,
        80,
        60,
        0,
        0,
        0);
    CHECK(window != None);
    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_create(
              display,
              window,
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    composition = boxrig_x11_input_adapter_composition(adapter);
    CHECK(composition != NULL);

    memset(&text, 0, sizeof(text));
    text.length = sizeof(initial_characters) / sizeof(initial_characters[0]);
    text.feedback = initial_feedback;
    text.encoding_is_wchar = True;
    text.string.wide_char = initial_characters;
    memset(&draw, 0, sizeof(draw));
    draw.caret = (int)text.length;
    draw.text = &text;
    CHECK(boxrig_x11_input_adapter_preedit_start(adapter) == 0);
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);

    memset(&feedback_update, 0, sizeof(feedback_update));
    feedback_update.length =
        sizeof(target_feedback) / sizeof(target_feedback[0]);
    feedback_update.feedback = target_feedback;
    feedback_update.encoding_is_wchar = True;
    feedback_update.string.wide_char = NULL;
    draw.chg_first = 1;
    draw.chg_length = 0;
    draw.caret = 3;
    draw.text = &feedback_update;
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.text_length == 12);
    CHECK(view.selection_offset == 3 && view.selection_length == 6);
    CHECK(view.segment_count == 3);
    CHECK(view.segments[1].text_offset == 3 &&
          view.segments[1].text_length == 6);
    CHECK((view.segments[1].feedback &
           BOXRIG_COMPOSITION_FEEDBACK_TARGET) != 0);

    memset(&text, 0, sizeof(text));
    text.length = 1;
    text.feedback = NULL;
    text.encoding_is_wchar = True;
    text.string.wide_char = inserted_character;
    draw.chg_first = 2;
    draw.chg_length = 0;
    draw.caret = 3;
    draw.text = &text;
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.text_length == sizeof(expected_after_insert) - 1);
    CHECK(memcmp(
              view.text,
              expected_after_insert,
              view.text_length) == 0);
    CHECK(view.selection_offset == 3 && view.selection_length == 9);

    draw.chg_first = 3;
    draw.chg_length = 1;
    draw.caret = 3;
    draw.text = NULL;
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.text_length == sizeof(expected_after_delete) - 1);
    CHECK(memcmp(
              view.text,
              expected_after_delete,
              view.text_length) == 0);
    CHECK(view.selection_offset == 3 && view.selection_length == 6);

    feedback_update.length =
        sizeof(converted_feedback) / sizeof(converted_feedback[0]);
    feedback_update.feedback = converted_feedback;
    draw.chg_first = 1;
    draw.chg_length = 0;
    draw.caret = 0;
    draw.text = &feedback_update;
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    feedback_update.length =
        sizeof(final_target_feedback) / sizeof(final_target_feedback[0]);
    feedback_update.feedback = final_target_feedback;
    draw.chg_first = 3;
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.segment_count == 3);
    CHECK(view.selection_offset == 9 && view.selection_length == 3);
    CHECK(view.caret_offset == 0 && view.caret_visible);

    memset(&caret, 0, sizeof(caret));
    caret.direction = XIMForwardWord;
    caret.style = XIMIsPrimary;
    CHECK(boxrig_x11_input_adapter_preedit_caret(adapter, &caret) == 0);
    CHECK(caret.position == 1);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.caret_offset == 3);
    CHECK(boxrig_x11_input_adapter_preedit_caret(adapter, &caret) == 0);
    CHECK(caret.position == 3);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.caret_offset == 9);

    caret.direction = XIMBackwardWord;
    CHECK(boxrig_x11_input_adapter_preedit_caret(adapter, &caret) == 0);
    CHECK(caret.position == 1);
    caret.direction = XIMDontChange;
    caret.style = XIMIsInvisible;
    CHECK(boxrig_x11_input_adapter_preedit_caret(adapter, &caret) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.caret_offset == 3 && !view.caret_visible);

    caret.direction = XIMLineEnd;
    caret.style = XIMIsSecondary;
    CHECK(boxrig_x11_input_adapter_preedit_caret(adapter, &caret) == 0);
    CHECK(caret.position == 4);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.caret_offset == sizeof(expected_after_delete) - 1);
    CHECK(view.caret_visible);
    caret.direction = XIMForwardChar;
    CHECK(boxrig_x11_input_adapter_preedit_caret(adapter, &caret) == 0);
    CHECK(caret.position == 4);
    caret.direction = XIMBackwardChar;
    CHECK(boxrig_x11_input_adapter_preedit_caret(adapter, &caret) == 0);
    CHECK(caret.position == 3);
    CHECK(boxrig_x11_input_adapter_preedit_caret(adapter, &caret) == 0);
    CHECK(caret.position == 2);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.caret_offset == 6);
    caret.direction = XIMLineStart;
    CHECK(boxrig_x11_input_adapter_preedit_caret(adapter, &caret) == 0);
    CHECK(caret.position == 0);
    caret.direction = XIMBackwardChar;
    CHECK(boxrig_x11_input_adapter_preedit_caret(adapter, &caret) == 0);
    CHECK(caret.position == 0);
    caret.direction = XIMAbsolutePosition;
    caret.position = 2;
    CHECK(boxrig_x11_input_adapter_preedit_caret(adapter, &caret) == 0);
    CHECK(caret.position == 2);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.caret_offset == 6);
    caret.position = 5;
    CHECK(boxrig_x11_input_adapter_preedit_caret(adapter, &caret) == -1);
    caret.position = -1;
    CHECK(boxrig_x11_input_adapter_preedit_caret(adapter, &caret) == -1);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.caret_offset == 6 && view.caret_visible);

    boxrig_x11_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    (void)XSetLocaleModifiers("");
    return true;
}

static bool test_xim_multibyte_limits_and_malformed_updates_are_transactional(
    void)
{
    static const char multibyte_text[] = "\343\201\213A";
    static const char malformed_text[] = "\303(";
    Display *display;
    Window window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_x11_input_adapter *adapter = NULL;
    boxrig_composition *composition;
    boxrig_composition_view view;
    XIMText text;
    XIMPreeditDrawCallbackStruct draw;
    char maximum_text[4097];
    char oversized_text[4098];
    uint64_t accepted_revision;

    CHECK(setlocale(LC_CTYPE, "C.UTF-8") != NULL);
    CHECK(XSetLocaleModifiers("@im=none") != NULL);
    display = XOpenDisplay(NULL);
    CHECK(display != NULL);
    window = XCreateSimpleWindow(
        display,
        RootWindow(display, DefaultScreen(display)),
        0,
        0,
        80,
        60,
        0,
        0,
        0);
    CHECK(window != None);
    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_create(
              display,
              window,
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    composition = boxrig_x11_input_adapter_composition(adapter);
    CHECK(composition != NULL);
    CHECK(boxrig_x11_input_adapter_preedit_start(adapter) == 0);

    memset(&text, 0, sizeof(text));
    text.length = 2;
    text.encoding_is_wchar = False;
    text.string.multi_byte = (char *)multibyte_text;
    memset(&draw, 0, sizeof(draw));
    draw.caret = 2;
    draw.text = &text;
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.text_length == sizeof(multibyte_text) - 1);
    CHECK(memcmp(view.text, multibyte_text, view.text_length) == 0);
    CHECK(view.caret_offset == sizeof(multibyte_text) - 1);
    accepted_revision = view.revision;

    text.length = 1;
    text.string.multi_byte = (char *)malformed_text;
    draw.chg_length = 2;
    draw.caret = 0;
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == -1);
    draw.chg_first = -1;
    draw.chg_length = 0;
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == -1);
    draw.chg_first = 0;
    draw.chg_length = -1;
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == -1);
    draw.chg_length = 0;
    draw.caret = -1;
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == -1);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.revision == accepted_revision);
    CHECK(view.text_length == sizeof(multibyte_text) - 1);
    CHECK(memcmp(view.text, multibyte_text, view.text_length) == 0);

    wchar_t invalid_characters[] = {(wchar_t)-1, 0xd800, 0xdfff, 0x110000};
    text.encoding_is_wchar = True;
    draw.chg_length = 2;
    draw.caret = 0;
    for (size_t index = 0;
         index < sizeof(invalid_characters) / sizeof(invalid_characters[0]);
         index++) {
        text.string.wide_char = &invalid_characters[index];
        CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == -1);
        CHECK(boxrig_composition_get_view(composition, &view) ==
              BOXRIG_COMPOSITION_OK);
        CHECK(view.revision == accepted_revision);
        CHECK(view.text_length == sizeof(multibyte_text) - 1);
        CHECK(memcmp(view.text, multibyte_text, view.text_length) == 0);
    }

    memset(maximum_text, 'a', sizeof(maximum_text) - 1);
    maximum_text[sizeof(maximum_text) - 1] = '\0';
    memset(&text, 0, sizeof(text));
    text.length = sizeof(maximum_text) - 1;
    text.encoding_is_wchar = False;
    text.string.multi_byte = maximum_text;
    draw.chg_first = 0;
    draw.chg_length = 2;
    draw.caret = sizeof(maximum_text) - 1;
    draw.text = &text;
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.text_length == sizeof(maximum_text) - 1);
    CHECK(memcmp(view.text, maximum_text, view.text_length) == 0);
    CHECK(view.caret_offset == sizeof(maximum_text) - 1);
    accepted_revision = view.revision;

    memset(oversized_text, 'b', sizeof(oversized_text) - 1);
    oversized_text[sizeof(oversized_text) - 1] = '\0';
    text.length = sizeof(oversized_text) - 1;
    text.string.multi_byte = oversized_text;
    draw.chg_length = sizeof(maximum_text) - 1;
    draw.caret = sizeof(oversized_text) - 1;
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == -1);
    draw.chg_first = sizeof(maximum_text);
    draw.chg_length = 0;
    draw.caret = 0;
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == -1);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.revision == accepted_revision);
    CHECK(view.text_length == sizeof(maximum_text) - 1);
    CHECK(memcmp(view.text, maximum_text, view.text_length) == 0);

    boxrig_x11_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    (void)XSetLocaleModifiers("");
    return true;
}

static bool test_ibus_preedit_lifecycle_survives_backspace(void)
{
    static const uint8_t expected_three[] = {
        0xe3, 0x81, 0x93, 0xe3, 0x82, 0x8c, 0xe3, 0x81, 0xaf};
    static const uint8_t expected_two[] = {
        0xe3, 0x81, 0x93, 0xe3, 0x82, 0x8c};
    static const uint8_t expected_one[] = {0xe3, 0x81, 0x93};
    static const uint8_t expected_restart[] = {0xe3, 0x81, 0x8d};
    Display *display;
    Window window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_x11_input_adapter *adapter = NULL;
    boxrig_composition *composition;
    boxrig_composition_view view;
    wchar_t three_characters[] = {0x3053, 0x308c, 0x306f};
    wchar_t two_characters[] = {0x3053, 0x308c};
    wchar_t one_character[] = {0x3053};
    wchar_t restart_character[] = {0x304d};
    XIMFeedback feedback[] = {XIMUnderline, XIMUnderline, XIMUnderline};
    XIMText text;
    XIMPreeditDrawCallbackStruct draw;
    XEvent key_event;
    const uint8_t *output = NULL;
    size_t output_length;

    (void)setlocale(LC_CTYPE, "");
    CHECK(XSetLocaleModifiers("@im=none") != NULL);
    display = XOpenDisplay(NULL);
    CHECK(display != NULL);
    window = XCreateSimpleWindow(
        display,
        RootWindow(display, DefaultScreen(display)),
        0,
        0,
        80,
        60,
        0,
        0,
        0);
    CHECK(window != None);
    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_create(
              display,
              window,
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    composition = boxrig_x11_input_adapter_composition(adapter);
    CHECK(composition != NULL);
    memset(&text, 0, sizeof(text));
    text.length = 3;
    text.feedback = feedback;
    text.encoding_is_wchar = True;
    text.string.wide_char = three_characters;
    memset(&draw, 0, sizeof(draw));
    draw.caret = 3;
    draw.text = &text;

    CHECK(boxrig_x11_input_adapter_preedit_start(adapter) == 0);
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active && view.text_length == sizeof(expected_three));
    CHECK(memcmp(view.text, expected_three, view.text_length) == 0);

    text.length = 2;
    text.string.wide_char = two_characters;
    draw.chg_length = 3;
    draw.caret = 2;
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active && view.text_length == sizeof(expected_two));
    CHECK(memcmp(view.text, expected_two, view.text_length) == 0);

    text.length = 1;
    text.string.wide_char = one_character;
    draw.chg_length = 2;
    draw.caret = 1;
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active && view.text_length == sizeof(expected_one));
    CHECK(memcmp(view.text, expected_one, view.text_length) == 0);

    CHECK(boxrig_x11_input_adapter_preedit_done(adapter) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active);

    text.length = 1;
    text.string.wide_char = restart_character;
    draw.chg_length = 1;
    draw.caret = 1;
    draw.text = &text;
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active && view.text_length == sizeof(expected_restart));
    CHECK(memcmp(view.text, expected_restart, view.text_length) == 0);

    draw.chg_length = 1;
    draw.caret = 0;
    draw.text = NULL;
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active && view.text_length == 0 && view.caret_offset == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_x11_input_adapter_preedit_done(adapter) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active);

    text.string.wide_char = one_character;
    draw.chg_length = 0;
    draw.caret = 1;
    draw.text = &text;
    CHECK(boxrig_x11_input_adapter_preedit_start(adapter) == 0);
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    intercepted_lookup_both_keycode =
        XKeysymToKeycode(display, XK_BackSpace);
    CHECK(intercepted_lookup_both_keycode != 0);
    memset(&key_event, 0, sizeof(key_event));
    key_event.xkey.type = KeyPress;
    key_event.xkey.display = display;
    key_event.xkey.window = window;
    key_event.xkey.root = RootWindow(display, DefaultScreen(display));
    key_event.xkey.keycode = intercepted_lookup_both_keycode;
    key_event.xkey.time = CurrentTime;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &key_event) == 1);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active);
    CHECK(view.last_event == BOXRIG_COMPOSITION_PREEDIT_COMPLETE);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == 1 && output[0] == 0x7f);
    key_event.xkey.type = KeyRelease;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &key_event) == 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == output_length);

    text.string.wide_char = restart_character;
    draw.chg_length = 1;
    draw.caret = 1;
    draw.text = &text;
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active && view.text_length == sizeof(expected_restart));
    CHECK(memcmp(view.text, expected_restart, view.text_length) == 0);
    intercepted_lookup_both_keycode = 0;

    intercepted_filtered_keycode =
        XKeysymToKeycode(display, XK_BackSpace);
    CHECK(intercepted_filtered_keycode != 0);
    key_event.xkey.type = KeyPress;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &key_event) == 1);
    key_event.xkey.type = KeyRelease;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &key_event) == 1);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active && view.text_length == 0);
    CHECK(view.last_event == BOXRIG_COMPOSITION_CANCEL);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == output_length);
    intercepted_filtered_keycode = 0;

    boxrig_x11_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    (void)XSetLocaleModifiers("");
    return true;
}

static bool test_escape_that_ends_xim_preedit_does_not_reach_terminal(void)
{
    wchar_t character[] = {0x304d};
    Display *display;
    Window window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_x11_input_adapter *adapter = NULL;
    boxrig_composition *composition;
    boxrig_composition_view view;
    XIMFeedback feedback[] = {XIMUnderline};
    XIMText text;
    XIMPreeditDrawCallbackStruct draw;
    XIMPreeditDrawCallbackStruct empty_draw;
    XEvent event;
    const uint8_t *output = NULL;

    (void)setlocale(LC_CTYPE, "");
    CHECK(XSetLocaleModifiers("@im=none") != NULL);
    display = XOpenDisplay(NULL);
    CHECK(display != NULL);
    window = XCreateSimpleWindow(
        display,
        RootWindow(display, DefaultScreen(display)),
        0,
        0,
        80,
        60,
        0,
        0,
        0);
    CHECK(window != None);
    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_create(
              display,
              window,
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    composition = boxrig_x11_input_adapter_composition(adapter);
    CHECK(composition != NULL);
    memset(&text, 0, sizeof(text));
    text.length = 1;
    text.feedback = feedback;
    text.encoding_is_wchar = True;
    text.string.wide_char = character;
    memset(&draw, 0, sizeof(draw));
    draw.caret = 1;
    draw.text = &text;
    CHECK(boxrig_x11_input_adapter_preedit_start(adapter) == 0);
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);

    intercepted_preedit_done_keycode =
        XKeysymToKeycode(display, XK_Escape);
    CHECK(intercepted_preedit_done_keycode != 0);
    intercepted_preedit_done_adapter = adapter;
    intercepted_preedit_done_failed = false;
    memset(&event, 0, sizeof(event));
    event.xkey.type = KeyPress;
    event.xkey.display = display;
    event.xkey.window = window;
    event.xkey.root = RootWindow(display, DefaultScreen(display));
    event.xkey.keycode = intercepted_preedit_done_keycode;
    event.xkey.time = CurrentTime;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(!intercepted_preedit_done_failed);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active);
    CHECK(view.last_event == BOXRIG_COMPOSITION_CANCEL);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    event.xkey.type = KeyRelease;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    intercepted_preedit_done_keycode = 0;
    intercepted_preedit_done_adapter = NULL;
    CHECK(boxrig_x11_input_adapter_preedit_start(adapter) == 0);
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    intercepted_empty_preedit_keycode =
        XKeysymToKeycode(display, XK_Escape);
    CHECK(intercepted_empty_preedit_keycode != 0);
    intercepted_empty_preedit_adapter = adapter;
    intercepted_empty_preedit_failed = false;
    event.xkey.type = KeyPress;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(!intercepted_empty_preedit_failed);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active);
    CHECK(view.last_event == BOXRIG_COMPOSITION_CANCEL);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    event.xkey.type = KeyRelease;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    intercepted_empty_preedit_keycode = 0;
    intercepted_empty_preedit_adapter = NULL;
    CHECK(boxrig_x11_input_adapter_preedit_start(adapter) == 0);
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    intercepted_filtered_keycode =
        XKeysymToKeycode(display, XK_Escape);
    CHECK(intercepted_filtered_keycode != 0);
    event.xkey.type = KeyPress;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active);
    CHECK(view.last_event == BOXRIG_COMPOSITION_CANCEL);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    event.xkey.type = KeyRelease;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    memset(&empty_draw, 0, sizeof(empty_draw));
    empty_draw.chg_length = 1;
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &empty_draw) == 0);
    CHECK(boxrig_x11_input_adapter_preedit_done(adapter) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active);
    CHECK(view.last_event == BOXRIG_COMPOSITION_CANCEL);

    feedback[0] = XIMPrimary;
    CHECK(boxrig_x11_input_adapter_preedit_start(adapter) == 0);
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    event.xkey.type = KeyPress;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active);
    CHECK((view.segments[0].feedback &
           BOXRIG_COMPOSITION_FEEDBACK_CONVERTED) != 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    event.xkey.type = KeyRelease;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    feedback[0] = XIMUnderline;
    draw.chg_length = 1;
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active);
    event.xkey.type = KeyPress;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active);
    CHECK(view.last_event == BOXRIG_COMPOSITION_CANCEL);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    event.xkey.type = KeyRelease;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    intercepted_filtered_keycode = 0;
    draw.chg_length = 0;
    feedback[0] = XIMPrimary;
    CHECK(boxrig_x11_input_adapter_preedit_start(adapter) == 0);
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &draw) == 0);
    intercepted_passthrough_keycode =
        XKeysymToKeycode(display, XK_Escape);
    CHECK(intercepted_passthrough_keycode != 0);
    event.xkey.type = KeyPress;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active && view.text_length == 3);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    memset(&empty_draw, 0, sizeof(empty_draw));
    empty_draw.chg_length = 1;
    CHECK(boxrig_x11_input_adapter_preedit_draw(adapter, &empty_draw) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active);
    CHECK(view.last_event == BOXRIG_COMPOSITION_CANCEL);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    event.xkey.type = KeyRelease;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    intercepted_passthrough_keycode = 0;
    event.xkey.type = KeyPress;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(output[0] == 0x1b);
    event.xkey.type = KeyRelease;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);

    boxrig_x11_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    (void)XSetLocaleModifiers("");
    return true;
}


static bool test_terminal_io_configuration(void)
{
    Display *display = XOpenDisplay(NULL);
    CHECK(display != NULL);
    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 80, 60, 0, 0, 0);
    CHECK(window != None);
    for (unsigned encoding = 0; encoding < 2; encoding++) {

        const uint8_t mapped[] = {0x1b, 'U', 0};
        const boxrig_key_mapping mapping = {BOXRIG_KEY_UP, 0, mapped, sizeof(mapped)};
        boxrig_terminal_config config = boxrig_terminal_config_default();
        config.encoding = (boxrig_encoding)encoding;
        config.input.enter_newline = BOXRIG_NEWLINE_LF;
        config.paste_newline = BOXRIG_NEWLINE_CRLF;
        config.input.key_mappings = &mapping;
        config.input.key_mapping_count = 1;
        boxrig_terminal *terminal = NULL;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);

        boxrig_x11_input_adapter *adapter = NULL;
        CHECK(boxrig_x11_input_adapter_create(display, window, window, terminal,
                  10, 20, 1024, config.output_capacity, &adapter) == 0);
        CHECK(boxrig_x11_input_adapter_preedit_start(adapter) == 0);
        CHECK(boxrig_x11_input_adapter_commit_utf8(adapter, "日本", 6) == 0);
        XEvent event = {0};
        event.xkey.display = display;
        event.xkey.window = window;
        event.xkey.root = DefaultRootWindow(display);
        const KeySym keys[] = {XK_Up, XK_Return};
        for (unsigned index = 0; index < 2; index++) {
            event.xkey.keycode = XKeysymToKeycode(display, keys[index]);
            event.xkey.type = KeyPress;
            CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
            event.xkey.type = KeyRelease;
            CHECK(boxrig_x11_input_adapter_process_event(adapter, &event) == 1);
        }
        CHECK(boxrig_x11_input_adapter_queue_paste(adapter, (const uint8_t *)"日本\n", 7) == 0);

        const uint8_t utf8_expected[] = "日本\033U\0\n日本\r\n";
        const uint8_t sjis_expected[] = "\x93\xfa\x96\x7b\033U\0\n\x93\xfa\x96\x7b\r\n";
        const uint8_t *expected = encoding ? sjis_expected : utf8_expected;
        size_t length = encoding ? sizeof(sjis_expected) - 1 : sizeof(utf8_expected) - 1;
        const uint8_t *output;
        CHECK(boxrig_terminal_output_peek(terminal, &output) == length);
        CHECK(memcmp(output, expected, length) == 0);

        boxrig_x11_input_adapter_destroy(adapter);
        boxrig_terminal_destroy(terminal);
    }
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return true;
}

static bool test_input_inhibition(void)
{
    Display *display = XOpenDisplay(NULL);
    CHECK(display != NULL);
    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 80, 60, 0, 0, 0);
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.output_capacity = 6;
    boxrig_terminal *terminal = NULL;
    boxrig_x11_input_adapter *adapter = NULL;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_create(display, window, window, terminal,
        8, 16, 1024, 1, &adapter) == 0);
    boxrig_ui_input_state state;
    CHECK(boxrig_x11_input_adapter_get_input_state(adapter, &state) == BOXRIG_UI_OK);
    CHECK(state.phase == BOXRIG_UI_INPUT_ENABLED);
    uint64_t before = state.generation;
    CHECK(boxrig_terminal_send_text(terminal, "filled", 6) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_queue_paste(adapter, (const uint8_t *)"discard", 7) == 0);
    CHECK(boxrig_x11_input_adapter_set_input_enabled(adapter, false) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_input_adapter_get_input_state(adapter, &state) == BOXRIG_UI_OK);
    CHECK(state.phase == BOXRIG_UI_INPUT_DISABLED && state.generation > before);
    CHECK(boxrig_x11_input_adapter_queue_paste(adapter, (const uint8_t *)"blocked", 7) == -1);
    CHECK(boxrig_x11_input_adapter_preedit_start(adapter) == 0);
    boxrig_composition_view composition;
    CHECK(boxrig_composition_get_view(boxrig_x11_input_adapter_composition(adapter), &composition) == BOXRIG_COMPOSITION_OK);
    CHECK(!composition.active);
    CHECK(boxrig_x11_input_adapter_commit_utf8(adapter, "old", 3) == 0);
    const uint8_t *output;
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 6 && memcmp(output, "filled", 6) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 6) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_retry(adapter) == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_x11_input_adapter_set_input_enabled(adapter, true) == BOXRIG_UI_OK);

    XEvent key = {0};
    key.xkey.display = display;
    key.xkey.window = window;
    key.xkey.root = DefaultRootWindow(display);
    key.xkey.keycode = XKeysymToKeycode(display, XK_Return);
    key.xkey.type = KeyPress;
    CHECK(boxrig_terminal_send_text(terminal, "filled", 6) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &key) == 1);
    CHECK(boxrig_x11_input_adapter_blocked(adapter));
    CHECK(boxrig_x11_input_adapter_set_input_enabled(adapter, false) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_input_adapter_set_input_enabled(adapter, true) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_output_consume(terminal, 6) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_retry(adapter) == 0);
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &key) == 1);
    key.xkey.type = KeyRelease;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &key) == 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    key.xkey.type = KeyPress;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &key) == 1);
    key.xkey.type = KeyRelease;
    CHECK(boxrig_x11_input_adapter_process_event(adapter, &key) == 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1 && output[0] == '\r');
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);

    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\x1b[?2004h", 8, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_queue_paste(adapter, (const uint8_t *)"discard", 7) == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 6 && memcmp(output, "\x1b[200~", 6) == 0);
    CHECK(boxrig_x11_input_adapter_set_input_enabled(adapter, false) == BOXRIG_UI_BUSY);
    CHECK(boxrig_x11_input_adapter_get_input_state(adapter, &state) == BOXRIG_UI_OK);
    CHECK(state.phase == BOXRIG_UI_INPUT_STOPPING);
    CHECK(boxrig_x11_input_adapter_set_input_enabled(adapter, true) == BOXRIG_UI_BUSY);
    CHECK(boxrig_x11_input_adapter_set_input_config(adapter, &config.input) == BOXRIG_UI_BUSY);
    CHECK(boxrig_terminal_output_consume(terminal, 6) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\x1b[5n", 4, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_retry(adapter) == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 4 && memcmp(output, "\x1b[0n", 4) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 4) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_retry(adapter) == 0);
    CHECK(boxrig_x11_input_adapter_get_input_state(adapter, &state) == BOXRIG_UI_OK);
    CHECK(state.phase == BOXRIG_UI_INPUT_DISABLED);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 6 && memcmp(output, "\x1b[201~", 6) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 6) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_input_adapter_set_input_enabled(adapter, true) == BOXRIG_UI_OK);
    boxrig_x11_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return true;
}

int main(void)
{
    static const struct {
        const char *name;
        bool (*run)(void);
    } tests[] = {
        {"terminal UTF-8/Shift_JIS IME, key and paste settings", test_terminal_io_configuration},
        {"input inhibition and started paste completion", test_input_inhibition},
        {"XIM protocol events are filtered",
         test_xim_protocol_events_are_filtered},
        {"XIM functional key commits text",
         test_xim_functional_key_commits_text},
        {"application key routing captures the key series",
         test_application_key_routing_captures_the_key_series},
        {"XKB identity distinguishes active and base layouts",
         test_xkb_identity_distinguishes_active_and_base_layouts},
        {"application mouse routing captures the drag series",
         test_application_mouse_routing_captures_the_drag_series},
        {"key focus and mouse events", test_key_focus_and_mouse_events},
        {"pending key retries atomically", test_pending_key_retries_atomically},
        {"streamed paste normalizes and retries",
         test_streamed_paste_normalizes_and_retries},
        {"primary selection paste", test_primary_selection_paste},
        {"copy ownership TARGETS direct INCR and limits",
         test_copy_ownership_targets_direct_incr_and_limits},
        {"INCR paste owner loss and capacity",
         test_incr_paste_owner_loss_and_capacity},
        {"XIM callbacks follow the common composition fixture",
         test_xim_callbacks_follow_the_common_composition_fixture},
        {"XIM clause feedback partial edits and caret moves",
         test_xim_clause_feedback_partial_edits_and_caret_moves},
        {"XIM multibyte limits and malformed updates are transactional",
         test_xim_multibyte_limits_and_malformed_updates_are_transactional},
        {"IBus preedit lifecycle survives backspace",
         test_ibus_preedit_lifecycle_survives_backspace},
        {"Escape that ends XIM preedit does not reach terminal",
         test_escape_that_ends_xim_preedit_does_not_reach_terminal},
    };
    Display *sentinel_display = XOpenDisplay(NULL);
    size_t index;

    if (sentinel_display == NULL) {
        fprintf(stderr, "could not keep X display active\n");
        return 1;
    }
    for (index = 0; index < sizeof(tests) / sizeof(tests[0]); index++) {
        if (!tests[index].run()) {
            fprintf(stderr, "Test failed: %s\n", tests[index].name);
            XCloseDisplay(sentinel_display);
            return 1;
        }
        printf("PASS %s\n", tests[index].name);
    }
    XCloseDisplay(sentinel_display);
    return 0;
}
