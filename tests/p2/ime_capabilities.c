/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <X11/Xlib.h>
#include <wayland-client.h>
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "boxrig/x11/input.h"

static int inspect_xim(void)
{
    if (setlocale(LC_ALL, "") == NULL || XSetLocaleModifiers("") == NULL)
        return 1;
    Display *display = XOpenDisplay(NULL);
    if (display == NULL) return 1;
    XIM im = XOpenIM(display, NULL, NULL, NULL);
    if (im == NULL) {
        XCloseDisplay(display);
        return 1;
    }
    XIMValuesList *values = NULL;
    XIMStyles *styles = NULL;
    char *error = XGetIMValues(im, XNQueryInputStyle, &styles,
                              XNQueryICValuesList, &values, NULL);
    bool conversion = false, callback = false;
    if (error == NULL && values != NULL) {
        for (unsigned short i = 0; i < values->count_values; i++) {
            const char *value = values->supported_values[i];
            printf("xim value=%s\n", value);
            conversion |= strcmp(value, XNStringConversion) == 0;
            callback |= strcmp(value, XNStringConversionCallback) == 0;
        }
    }
    printf("xim query=%s string_conversion=%d string_conversion_callback=%d\n",
        error == NULL ? "supported" : "unsupported", conversion, callback);
    XFree(values);
    XFree(styles);
    XCloseIM(im);
    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display),
                                         0, 0, 80, 60, 0, 0, 0);
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_x11_input_adapter *adapter = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    int status = 1;
    if (window == None || boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        boxrig_ui_model_create(terminal, &model) != BOXRIG_UI_OK ||
        boxrig_ui_view_create(model, &view_config, &view) != BOXRIG_UI_OK ||
        boxrig_x11_input_adapter_create(display, window, window, terminal,
            10, 20, 1024, 128, &adapter) != 0) goto cleanup;
    boxrig_ui_editable_document *document = boxrig_ui_view_editable_document(view);
    boxrig_ui_editable_context_input context = {1, 1, "前かな後", 12, 0, 9, 9};
    if (boxrig_ui_editable_document_publish_context(document, &context) != BOXRIG_UI_OK ||
        boxrig_x11_input_adapter_set_editable_document(adapter, document) != 0) goto cleanup;
    bool native_supported = boxrig_x11_input_adapter_string_conversion_supported(adapter);
    boxrig_ui_editable_capabilities capabilities = boxrig_ui_editable_document_capabilities(document);
    printf("boxrig xim native_string_conversion=%d document_context=%d synchronous_owner=%d\n",
        native_supported, capabilities.context_available, capabilities.synchronous_edit_available);
    if ((!callback && native_supported) || !capabilities.context_available ||
        capabilities.synchronous_edit_available) goto cleanup;
    status = 0;
cleanup:
    boxrig_x11_input_adapter_destroy(adapter);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    if (window != None) XDestroyWindow(display, window);
    XCloseDisplay(display);
    return status;
}

static void global(
    void *data, struct wl_registry *registry, uint32_t name,
    const char *interface, uint32_t version)
{
    (void)registry;
    (void)name;
    bool *text_input_v3 = data;
    printf("wayland interface=%s version=%u\n", interface, version);
    if (strcmp(interface, "zwp_text_input_manager_v3") == 0)
        *text_input_v3 = true;
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static int inspect_wayland(void)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (display == NULL) return 1;
    struct wl_registry *registry = wl_display_get_registry(display);
    static const struct wl_registry_listener listener = {global, global_remove};
    bool text_input_v3 = false;
    wl_registry_add_listener(registry, &listener, &text_input_v3);
    int result = wl_display_roundtrip(display);
    printf("wayland text_input_v3=%d\n", text_input_v3);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return result < 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (strcmp(argv[1], "xim") == 0) return inspect_xim();
    if (strcmp(argv[1], "wayland") == 0) return inspect_wayland();
    return 2;
}
