/* boxrig - Under MIT. Observe background effects in the real sample host. */
#include "../../examples/boxrig_term/win32/application_internal.h"
#include <assert.h>

static boxrig_input_key_router original_router;
static bool initialized;
boxrig_ui_status __real_boxrig_win32_terminal_component_set_key_router(
    boxrig_win32_terminal_component *, boxrig_input_key_router, void *);

static boxrig_input_key_route route(const boxrig_key_event *event, void *context)
{
    boxrig_term_win32_application *owner = context;
    if (event->key == BOXRIG_KEY_F7 || event->key == BOXRIG_KEY_F8 || event->key == BOXRIG_KEY_F9) {
        if (event->action == BOXRIG_KEY_PRESS) {
            /* Feed Kitty graphics directly: system ConPTY strips APC strings.
             * The transport is irrelevant to the host composition under test. */
            if (!initialized || event->key == BOXRIG_KEY_F7) {
                static const uint8_t content[] =
                    "\033_Ga=d,d=A\033\\\033[0m\033[2J\033[H\033[38;2;255;255;255mBOXRIG foreground text\r\n"
                    "Acrylic affects the desktop behind this window.\r\n"
                    "\033_Gf=32,s=1,v=1,a=T,c=12,r=4;AP8A/w==\033\\"
                    "\033[8;1H\033]12;#FFFFFF\007\033[1 q";
                size_t consumed;
                assert(boxrig_terminal_feed(owner->terminal, content, sizeof(content) - 1,
                    &consumed) == BOXRIG_STATUS_OK && consumed == sizeof(content) - 1);
                /* Opaque application background under text/cursor isolates
                 * foreground antialiasing; the rest exposes the desktop. */
                uint8_t rgba[128 * 128 * 4] = {0};
                for (size_t y = 0; y < 128; y++)
                    for (size_t x = 0; x < 128; x++)
                        if (y < 12 || x < 4) rgba[(y * 128 + x) * 4 + 3] = 255;
                boxrig_ui_background_image image = {.width = 128, .height = 128,
                    .stride = 128 * 4, .length = sizeof(rgba), .rgba = rgba,
                    .opacity = 255, .fit = BOXRIG_UI_BACKGROUND_STRETCH};
                assert(boxrig_ui_view_set_background_image(
                    boxrig_win32_terminal_component_view(owner->component), &image) == BOXRIG_UI_OK);
                initialized = true;
            }
            if (event->key == BOXRIG_KEY_F7)
                return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
            bool enabled = event->key == BOXRIG_KEY_F9;
            assert(boxrig_win32_background_effect_set(owner->background_effect,
                enabled ? BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE : BOXRIG_UI_BACKGROUND_EFFECT_NONE) == BOXRIG_UI_OK);
            assert(SetWindowTextW(owner->window, enabled ? L"plan27-effect-on" : L"plan27-effect-off"));
        }
        return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
    }
    return original_router(event, context);
}

boxrig_ui_status __wrap_boxrig_win32_terminal_component_set_key_router(
    boxrig_win32_terminal_component *component, boxrig_input_key_router router, void *context)
{
    assert(original_router == NULL && router != NULL && context != NULL);
    original_router = router;
    return __real_boxrig_win32_terminal_component_set_key_router(component, route, context);
}
