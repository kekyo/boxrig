/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui.h"
#include "boxrig/x11.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>

#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

typedef struct test_compositor {
    Display *display;
    int screen;
    Window root;
    Atom selection;
    Window owner;
    bool redirected;
    int dirty_event_base;
} test_compositor;

static bool feed_text(
    boxrig_terminal *terminal,
    const uint8_t *text,
    size_t length)
{
    size_t consumed = 0;

    return boxrig_terminal_feed(terminal, text, length, &consumed) ==
               BOXRIG_STATUS_OK &&
           consumed == length;
}

static Window create_window(
    Display *display,
    int screen,
    unsigned int width,
    unsigned int height,
    const boxrig_x11_window_requirements *requirements)
{
    return XCreateWindow(
        display,
        RootWindow(display, screen),
        0,
        0,
        width,
        height,
        0,
        requirements->depth,
        InputOutput,
        requirements->visual,
        requirements->attribute_mask,
        (XSetWindowAttributes *)&requirements->attributes);
}

static bool test_compositor_initialize(
    Display *display,
    test_compositor *compositor)
{
    char selection_name[64];
    int dirty_error_base;
    int dirty_major = 0;
    int dirty_minor = 0;

    memset(compositor, 0, sizeof(*compositor));
    compositor->display = display;
    compositor->screen = DefaultScreen(display);
    compositor->root = RootWindow(display, compositor->screen);
    CHECK(XDamageQueryExtension(
              display,
              &compositor->dirty_event_base,
              &dirty_error_base) != 0);
    CHECK(XDamageQueryVersion(display, &dirty_major, &dirty_minor) != 0);
    CHECK(dirty_major > 0 || dirty_minor > 0);
    CHECK(snprintf(
              selection_name,
              sizeof(selection_name),
              "_NET_WM_CM_S%d",
              compositor->screen) > 0);
    compositor->selection = XInternAtom(display, selection_name, False);
    CHECK(compositor->selection != None);
    compositor->owner = XCreateSimpleWindow(
        display,
        compositor->root,
        0,
        0,
        1,
        1,
        0,
        0,
        0);
    CHECK(compositor->owner != None);
    XCompositeRedirectSubwindows(
        display,
        compositor->root,
        CompositeRedirectManual);
    XSync(display, False);
    compositor->redirected = true;
    return true;
}

static bool test_compositor_set_owner(
    test_compositor *compositor,
    bool owned)
{
    XSetSelectionOwner(
        compositor->display,
        compositor->selection,
        owned ? compositor->owner : None,
        CurrentTime);
    XSync(compositor->display, False);
    return XGetSelectionOwner(
               compositor->display,
               compositor->selection) ==
           (owned ? compositor->owner : None);
}

static void test_compositor_destroy(test_compositor *compositor)
{
    if (compositor->owner != None) {
        XSetSelectionOwner(
            compositor->display,
            compositor->selection,
            None,
            CurrentTime);
        XDestroyWindow(compositor->display, compositor->owner);
        compositor->owner = None;
    }
    if (compositor->redirected) {
        XCompositeUnredirectSubwindows(
            compositor->display,
            compositor->root,
            CompositeRedirectManual);
        compositor->redirected = false;
    }
    XSync(compositor->display, False);
}

static bool process_selection_change(
    Display *display,
    boxrig_x11_transparency_context *context,
    size_t expected_changes)
{
    size_t handled = 0;
    size_t changes = 0;

    XSync(display, False);
    while (XPending(display) != 0) {
        XEvent event;
        bool changed = false;
        int result;

        XNextEvent(display, &event);
        result = boxrig_x11_transparency_context_process_event(
            context,
            &event,
            &changed);
        CHECK(result >= 0);
        if (result == 1) {
            handled++;
            if (changed) {
                changes++;
            }
        }
    }
    CHECK(handled != 0);
    CHECK(changes == expected_changes);
    return true;
}

static unsigned long allocate_rgb(
    Display *display,
    Colormap colormap,
    uint8_t red,
    uint8_t green,
    uint8_t blue)
{
    XColor color;

    memset(&color, 0, sizeof(color));
    color.red = (unsigned short)((unsigned int)red * 257u);
    color.green = (unsigned short)((unsigned int)green * 257u);
    color.blue = (unsigned short)((unsigned int)blue * 257u);
    color.flags = DoRed | DoGreen | DoBlue;
    if (XAllocColor(display, colormap, &color) == 0) {
        return ULONG_MAX;
    }
    return color.pixel;
}

static bool fill_window(
    Display *display,
    Window window,
    unsigned long pixel,
    unsigned int width,
    unsigned int height)
{
    GC graphics = XCreateGC(display, window, 0, NULL);

    if (graphics == NULL) {
        return false;
    }
    XSetForeground(display, graphics, pixel);
    XFillRectangle(display, window, graphics, 0, 0, width, height);
    XFreeGC(display, graphics);
    XFlush(display);
    return true;
}

static uint32_t opaque_colormap_pixel(
    Display *display,
    Colormap colormap,
    unsigned long pixel)
{
    XColor color;

    memset(&color, 0, sizeof(color));
    color.pixel = pixel;
    if (XQueryColor(display, colormap, &color) == 0) {
        return 0;
    }
    return UINT32_C(0xff000000) |
           ((uint32_t)(color.red / 257u) << 16) |
           ((uint32_t)(color.green / 257u) << 8) |
           (uint32_t)(color.blue / 257u);
}

static bool wait_for_dirty(
    Display *display,
    int dirty_event_base,
    Damage expected_dirty)
{
    struct pollfd descriptor;

    XFlush(display);
    descriptor.fd = ConnectionNumber(display);
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    for (;;) {
        while (XPending(display) != 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == dirty_event_base + XDamageNotify &&
                ((XDamageNotifyEvent *)&event)->damage == expected_dirty) {
                return true;
            }
        }
        descriptor.revents = 0;
        if (poll(&descriptor, 1, 10000) <= 0) {
            return false;
        }
    }
}

static void discard_pending_events(Display *display)
{
    XSync(display, False);
    while (XPending(display) != 0) {
        XEvent event;

        XNextEvent(display, &event);
    }
}

static XImage *capture_window(
    Display *display,
    Window window,
    unsigned int width,
    unsigned int height)
{
    XSync(display, False);
    return XGetImage(
        display,
        window,
        0,
        0,
        width,
        height,
        AllPlanes,
        ZPixmap);
}

static XImage *capture_composited(
    Display *display,
    Window background,
    Window terminal_window,
    Window scrollbar_window,
    Visual *terminal_visual,
    unsigned int width,
    unsigned int height,
    int scrollbar_x)
{
    XRenderPictFormat *background_format = XRenderFindVisualFormat(
        display,
        DefaultVisual(display, DefaultScreen(display)));
    XRenderPictFormat *terminal_format = XRenderFindVisualFormat(
        display,
        terminal_visual);
    XRenderPictFormat *output_format = XRenderFindStandardFormat(
        display,
        PictStandardARGB32);
    Pixmap background_pixmap = None;
    Pixmap terminal_pixmap = None;
    Pixmap scrollbar_pixmap = None;
    Pixmap output_pixmap = None;
    Picture background_picture = None;
    Picture terminal_picture = None;
    Picture scrollbar_picture = None;
    Picture output_picture = None;
    XImage *image = NULL;

    if (background_format == NULL || terminal_format == NULL ||
        output_format == NULL) {
        return NULL;
    }
    background_pixmap = XCompositeNameWindowPixmap(display, background);
    terminal_pixmap = XCompositeNameWindowPixmap(display, terminal_window);
    output_pixmap = XCreatePixmap(
        display,
        RootWindow(display, DefaultScreen(display)),
        width,
        height,
        32);
    if (background_pixmap == None || terminal_pixmap == None ||
        output_pixmap == None) {
        goto cleanup;
    }
    background_picture = XRenderCreatePicture(
        display,
        background_pixmap,
        background_format,
        0,
        NULL);
    terminal_picture = XRenderCreatePicture(
        display,
        terminal_pixmap,
        terminal_format,
        0,
        NULL);
    output_picture = XRenderCreatePicture(
        display,
        output_pixmap,
        output_format,
        0,
        NULL);
    if (background_picture == None || terminal_picture == None ||
        output_picture == None) {
        goto cleanup;
    }
    XRenderComposite(
        display,
        PictOpSrc,
        background_picture,
        None,
        output_picture,
        0,
        0,
        0,
        0,
        0,
        0,
        width,
        height);
    XRenderComposite(
        display,
        PictOpOver,
        terminal_picture,
        None,
        output_picture,
        0,
        0,
        0,
        0,
        0,
        0,
        width,
        height);
    if (scrollbar_window != None) {
        scrollbar_pixmap = XCompositeNameWindowPixmap(
            display,
            scrollbar_window);
        if (scrollbar_pixmap == None) {
            goto cleanup;
        }
        scrollbar_picture = XRenderCreatePicture(
            display,
            scrollbar_pixmap,
            background_format,
            0,
            NULL);
        if (scrollbar_picture == None) {
            goto cleanup;
        }
        XRenderComposite(
            display,
            PictOpOver,
            scrollbar_picture,
            None,
            output_picture,
            0,
            0,
            0,
            0,
            scrollbar_x,
            0,
            boxrig_x11_scrollbar_pixel_width(),
            height);
    }
    XSync(display, False);
    image = XGetImage(
        display,
        output_pixmap,
        0,
        0,
        width,
        height,
        AllPlanes,
        ZPixmap);

cleanup:
    if (scrollbar_picture != None) {
        XRenderFreePicture(display, scrollbar_picture);
    }
    if (output_picture != None) {
        XRenderFreePicture(display, output_picture);
    }
    if (terminal_picture != None) {
        XRenderFreePicture(display, terminal_picture);
    }
    if (background_picture != None) {
        XRenderFreePicture(display, background_picture);
    }
    if (scrollbar_pixmap != None) {
        XFreePixmap(display, scrollbar_pixmap);
    }
    if (output_pixmap != None) {
        XFreePixmap(display, output_pixmap);
    }
    if (terminal_pixmap != None) {
        XFreePixmap(display, terminal_pixmap);
    }
    if (background_pixmap != None) {
        XFreePixmap(display, background_pixmap);
    }
    return image;
}

static bool find_opaque_pixel(
    XImage *image,
    unsigned int cell_width,
    unsigned int cell_height,
    size_t column,
    int *x_out,
    int *y_out)
{
    unsigned int y;

    for (y = 0; y < cell_height; y++) {
        unsigned int x;

        for (x = 0; x < cell_width; x++) {
            unsigned long pixel = XGetPixel(
                image,
                (int)(column * cell_width + x),
                (int)y);

            if (((pixel >> 24) & 0xffu) == 255u) {
                *x_out = (int)(column * cell_width + x);
                *y_out = (int)y;
                return true;
            }
        }
    }
    return false;
}

static bool image_pixel_equals(
    XImage *image,
    int x,
    int y,
    uint32_t expected)
{
    uint32_t actual = (uint32_t)XGetPixel(image, x, y);

    if (actual != expected) {
        fprintf(
            stderr,
            "pixel (%d,%d): expected 0x%08x, got 0x%08x\n",
            x,
            y,
            expected,
            actual);
        return false;
    }
    return true;
}

static bool send_terminal_pointer_event(
    Display *display,
    Window window,
    int x,
    int y)
{
    XEvent event;
    XEvent received;

    memset(&event, 0, sizeof(event));
    event.xbutton.type = ButtonPress;
    event.xbutton.display = display;
    event.xbutton.window = window;
    event.xbutton.root = RootWindow(display, DefaultScreen(display));
    event.xbutton.same_screen = True;
    event.xbutton.button = Button1;
    event.xbutton.x = x;
    event.xbutton.y = y;
    CHECK(XSendEvent(display, window, False, ButtonPressMask, &event) != 0);
    XSync(display, False);
    CHECK(XCheckTypedWindowEvent(
              display,
              window,
              ButtonPress,
              &received) != 0);
    CHECK(received.xbutton.x == x && received.xbutton.y == y);
    return true;
}

static bool process_scrollbar_pointer_event(
    Display *display,
    Window window,
    boxrig_x11_scrollbar *scrollbar)
{
    XEvent event;
    XEvent received;

    memset(&event, 0, sizeof(event));
    event.xbutton.type = ButtonPress;
    event.xbutton.display = display;
    event.xbutton.window = window;
    event.xbutton.root = RootWindow(display, DefaultScreen(display));
    event.xbutton.same_screen = True;
    event.xbutton.button = Button1;
    CHECK(XSendEvent(display, window, False, ButtonPressMask, &event) != 0);
    XSync(display, False);
    CHECK(XCheckTypedWindowEvent(
              display,
              window,
              ButtonPress,
              &received) != 0);
    CHECK(boxrig_x11_scrollbar_process_event(scrollbar, &received) == 1);
    return true;
}

static bool render_compositor_fixture(Display *display)
{
    static const uint8_t setup[] =
        "\x1b[?25l"
        "\x1b]10;#e0c080\x07"
        "\x1b]11;#204060\x07"
        "\x1b]12;#80e020\x07"
        "\x1b[1;2H\x1b[48;2;80;40;20m \x1b[0m"
        "\x1b[1;3H\x1b[38;2;20;60;100;48;2;9;8;7;7m \x1b[0m"
        "\x1b[1;4H\x1b[38;2;224;192;128m\xe2\x96\x88\x1b[0m"
        "\x1b[1;5H\x1b[38;2;224;192;128;4m \x1b[0m"
        "\x1b[1;6H"
        "\x1b_Ga=T,i=16001,f=32,s=1,v=1,c=1,r=1,z=0,C=1;"
        "yGQygA==\x1b\\"
        "\x1b[1;7H\x1b[?25h";
    static const boxrig_composition_segment composition_segment = {
        0,
        1,
        BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE,
    };
    int screen = DefaultScreen(display);
    Colormap default_colormap = DefaultColormap(display, screen);
    test_compositor compositor;
    boxrig_ui_transparency_config transparency_config =
        boxrig_ui_transparency_config_default();
    boxrig_x11_transparency_context *transparency = NULL;
    boxrig_x11_window_requirements requirements;
    boxrig_ui_transparency_state state;
    boxrig_x11_renderer *renderer = NULL;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_x11_scrollbar *scrollbar = NULL;
    boxrig_composition *composition = NULL;
    boxrig_composition_event composition_event;
    boxrig_x11_render_statistics statistics;
    Window background = None;
    Window terminal_window = None;
    Window scrollbar_window = None;
    Window query_root = None;
    Window query_parent = None;
    Window *children = NULL;
    unsigned int child_count = 0;
    Damage background_dirty = None;
    XImage *raw = NULL;
    XImage *composited_before = NULL;
    XImage *composited_after = NULL;
    unsigned long first_background_color;
    unsigned long second_background_color;
    uint32_t first_background_pixel;
    uint32_t second_background_pixel;
    uint32_t expected_default;
    uint32_t expected_sgr;
    uint32_t expected_reverse;
    uint32_t expected_image;
    uint32_t expected_cursor;
    uint32_t expected_decoration;
    unsigned int cell_width;
    unsigned int cell_height;
    unsigned int scrollbar_width;
    unsigned int width;
    unsigned int height;
    int scrollbar_x;
    int opaque_glyph_x = 0;
    int opaque_glyph_y = 0;
    uint64_t revision;
    bool success = false;

    CHECK(test_compositor_initialize(display, &compositor));
    transparency_config.request = BOXRIG_UI_TRANSPARENCY_PREFERRED;
    transparency_config.background_opacity = 0;
    CHECK(boxrig_x11_transparency_context_create(
              display,
              screen,
              &transparency_config,
              &transparency) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_transparency_context_get_state(transparency, &state) ==
          BOXRIG_UI_OK);
    CHECK(state.effective == BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);
    CHECK(test_compositor_set_owner(&compositor, true));
    CHECK(process_selection_change(display, transparency, 1));
    CHECK(boxrig_x11_transparency_context_get_state(transparency, &state) ==
          BOXRIG_UI_OK);
    CHECK(state.effective == BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA);
    CHECK(state.effective_background_opacity == 0);
    CHECK(boxrig_x11_transparency_context_get_window_requirements(
              transparency,
              &requirements) == BOXRIG_UI_OK);
    CHECK(requirements.premultiplied_argb32);

    background = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        0,
        0,
        640,
        160,
        0,
        0,
        0);
    terminal_window = create_window(display, screen, 640, 160, &requirements);
    CHECK(background != None && terminal_window != None);
    CHECK(boxrig_x11_transparency_context_bind_window(
              transparency,
              terminal_window) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_transparency_context_get_state(transparency, &state) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_x11_renderer_create(display, terminal_window, &renderer) == 0);
    CHECK(boxrig_x11_renderer_apply_transparency_state(renderer, &state) == 0);
    cell_width = boxrig_x11_renderer_cell_width(renderer);
    cell_height = boxrig_x11_renderer_cell_height(renderer);
    scrollbar_width = boxrig_x11_scrollbar_pixel_width();
    CHECK(cell_width > 1 && cell_height > 2 && scrollbar_width > 0);
    width = cell_width * 8 + scrollbar_width + 3;
    height = cell_height + 3;
    scrollbar_x = (int)(width - scrollbar_width);
    XResizeWindow(display, background, width, height);
    XResizeWindow(display, terminal_window, width, height);
    CHECK(boxrig_x11_renderer_set_surface_size(renderer, width, height) == 0);
    XSelectInput(display, terminal_window, ButtonPressMask);
    XMapWindow(display, background);
    XMapWindow(display, terminal_window);

    terminal_config.rows = 1;
    terminal_config.columns = 8;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(
              terminal,
              cell_width,
              cell_height) == BOXRIG_STATUS_OK);
    CHECK(feed_text(terminal, setup, sizeof(setup) - 1));
    CHECK(boxrig_x11_scrollbar_create(
              display,
              terminal_window,
              terminal,
              &scrollbar) == 0);
    CHECK(boxrig_x11_scrollbar_resize(scrollbar, width, height) == 0);
    CHECK(boxrig_x11_scrollbar_synchronize(scrollbar) == 0);
    CHECK(XQueryTree(
              display,
              terminal_window,
              &query_root,
              &query_parent,
              &children,
              &child_count) != 0);
    CHECK(child_count == 1);
    scrollbar_window = children[0];
    XFree(children);
    children = NULL;

    first_background_color = allocate_rgb(
        display,
        default_colormap,
        0x30,
        0x20,
        0x10);
    second_background_color = allocate_rgb(
        display,
        default_colormap,
        0x08,
        0x40,
        0x90);
    CHECK(first_background_color != ULONG_MAX);
    CHECK(second_background_color != ULONG_MAX);
    first_background_pixel = opaque_colormap_pixel(
        display,
        default_colormap,
        first_background_color);
    second_background_pixel = opaque_colormap_pixel(
        display,
        default_colormap,
        second_background_color);
    CHECK(fill_window(
        display,
        background,
        first_background_color,
        width,
        height));
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    XSync(display, False);

    raw = capture_window(display, terminal_window, width, height);
    CHECK(raw != NULL);
    CHECK((uint32_t)XGetPixel(
              raw,
              (int)(cell_width / 2),
              (int)(cell_height / 2)) == 0);
    CHECK(find_opaque_pixel(
        raw,
        cell_width,
        cell_height,
        3,
        &opaque_glyph_x,
        &opaque_glyph_y));
    CHECK(((uint32_t)XGetPixel(
               raw,
               (int)(6 * cell_width + cell_width / 2),
               0) >>
           24) == 255u);
    XDestroyImage(raw);
    raw = NULL;
    CHECK(send_terminal_pointer_event(
        display,
        terminal_window,
        (int)(cell_width / 2),
        (int)(cell_height / 2)));

    transparency_config.background_opacity = 128;
    CHECK(boxrig_x11_transparency_context_set_config(
              transparency,
              &transparency_config) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_transparency_context_get_state(transparency, &state) ==
          BOXRIG_UI_OK);
    CHECK(state.effective_background_opacity == 128);
    CHECK(boxrig_x11_renderer_apply_transparency_state(renderer, &state) == 0);
    boxrig_x11_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    CHECK(boxrig_x11_renderer_get_render_statistics(renderer, &statistics) ==
          0);
    CHECK(statistics.row_raster_count == 1);
    CHECK(statistics.cell_raster_count == 8);
    boxrig_x11_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    CHECK(boxrig_x11_renderer_get_render_statistics(renderer, &statistics) ==
          0);
    CHECK(statistics.row_raster_count == 0);

    expected_default = boxrig_ui_argb32_premultiply(0x20, 0x40, 0x60, 128);
    expected_sgr = boxrig_ui_argb32_premultiply(80, 40, 20, 128);
    expected_reverse = boxrig_ui_argb32_premultiply(20, 60, 100, 128);
    expected_decoration = boxrig_ui_argb32_premultiply(
        224,
        192,
        128,
        255);
    expected_cursor = boxrig_ui_argb32_premultiply(0x80, 0xe0, 0x20, 255);
    expected_image = boxrig_ui_argb32_source_over(
        boxrig_ui_argb32_premultiply(200, 100, 50, 128),
        expected_default);
    raw = capture_window(display, terminal_window, width, height);
    CHECK(raw != NULL);
    CHECK((uint32_t)XGetPixel(
              raw,
              (int)(cell_width / 2),
              (int)(cell_height / 2)) == expected_default);
    CHECK(image_pixel_equals(
        raw,
        (int)(cell_width + cell_width / 2),
        (int)(cell_height / 2),
        expected_sgr));
    CHECK((uint32_t)XGetPixel(
              raw,
              (int)(2 * cell_width + cell_width / 2),
              (int)(cell_height / 2)) == expected_reverse);
    CHECK((uint32_t)XGetPixel(
              raw,
              (int)(4 * cell_width + cell_width / 2),
              (int)cell_height - 2) == expected_decoration);
    CHECK((uint32_t)XGetPixel(
              raw,
              (int)(5 * cell_width + cell_width / 2),
              (int)(cell_height / 2)) == expected_image);
    CHECK((uint32_t)XGetPixel(
              raw,
              (int)(6 * cell_width + cell_width / 2),
              0) == expected_cursor);
    CHECK((uint32_t)XGetPixel(
              raw,
              (int)(8 * cell_width + 1),
              (int)(cell_height / 2)) == expected_default);
    CHECK((uint32_t)XGetPixel(
              raw,
              (int)(cell_width / 2),
              (int)cell_height + 1) == expected_default);
    CHECK(find_opaque_pixel(
        raw,
        cell_width,
        cell_height,
        3,
        &opaque_glyph_x,
        &opaque_glyph_y));
    XDestroyImage(raw);
    raw = NULL;

    CHECK(boxrig_composition_create(
              terminal,
              boxrig_composition_limits_default(),
              &composition) == BOXRIG_COMPOSITION_OK);
    memset(&composition_event, 0, sizeof(composition_event));
    composition_event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    composition_event.text = " ";
    composition_event.text_length = 1;
    composition_event.segments = &composition_segment;
    composition_event.segment_count = 1;
    composition_event.caret_offset = 1;
    composition_event.caret_visible = true;
    CHECK(boxrig_composition_apply_event(composition, &composition_event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);
    raw = capture_window(display, terminal_window, width, height);
    CHECK(raw != NULL);
    CHECK((uint32_t)XGetPixel(
              raw,
              (int)(6 * cell_width + cell_width / 2),
              (int)(cell_height / 2)) == expected_default);
    CHECK((uint32_t)XGetPixel(
              raw,
              (int)(6 * cell_width + cell_width / 2),
              (int)cell_height - 2) == expected_decoration);
    CHECK((uint32_t)XGetPixel(
              raw,
              (int)(7 * cell_width),
              (int)(cell_height / 2)) == expected_cursor);
    XDestroyImage(raw);
    raw = NULL;
    memset(&composition_event, 0, sizeof(composition_event));
    composition_event.type = BOXRIG_COMPOSITION_CANCEL;
    CHECK(boxrig_composition_apply_event(composition, &composition_event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);

    background_dirty = XDamageCreate(
        display,
        background,
        XDamageReportNonEmpty);
    CHECK(background_dirty != None);
    XDamageSubtract(display, background_dirty, None, None);
    discard_pending_events(display);
    composited_before = capture_composited(
        display,
        background,
        terminal_window,
        scrollbar_window,
        requirements.visual,
        width,
        height,
        scrollbar_x);
    CHECK(composited_before != NULL);
    CHECK((uint32_t)XGetPixel(
              composited_before,
              (int)(cell_width / 2),
              (int)(cell_height / 2)) ==
          boxrig_ui_argb32_source_over(
              expected_default,
              first_background_pixel));
    CHECK(((uint32_t)XGetPixel(
               composited_before,
               scrollbar_x,
               (int)(height / 2)) >>
           24) == 255u);

    boxrig_x11_renderer_reset_render_statistics(renderer);
    CHECK(fill_window(
        display,
        background,
        second_background_color,
        width,
        height));
    CHECK(wait_for_dirty(
        display,
        compositor.dirty_event_base,
        background_dirty));
    composited_after = capture_composited(
        display,
        background,
        terminal_window,
        scrollbar_window,
        requirements.visual,
        width,
        height,
        scrollbar_x);
    CHECK(composited_after != NULL);
    CHECK((uint32_t)XGetPixel(
              composited_after,
              (int)(cell_width / 2),
              (int)(cell_height / 2)) ==
          boxrig_ui_argb32_source_over(
              expected_default,
              second_background_pixel));
    CHECK(XGetPixel(
              composited_before,
              (int)(cell_width / 2),
              (int)(cell_height / 2)) !=
          XGetPixel(
              composited_after,
              (int)(cell_width / 2),
              (int)(cell_height / 2)));
    CHECK(XGetPixel(
              composited_before,
              opaque_glyph_x,
              opaque_glyph_y) ==
          XGetPixel(composited_after, opaque_glyph_x, opaque_glyph_y));
    CHECK(XGetPixel(
              composited_before,
              (int)(6 * cell_width + cell_width / 2),
              0) ==
          XGetPixel(
              composited_after,
              (int)(6 * cell_width + cell_width / 2),
              0));
    CHECK(XGetPixel(
              composited_before,
              scrollbar_x,
              (int)(height / 2)) ==
          XGetPixel(
              composited_after,
              scrollbar_x,
              (int)(height / 2)));
    CHECK(boxrig_x11_renderer_get_render_statistics(renderer, &statistics) ==
          0);
    CHECK(statistics.row_raster_count == 0);
    CHECK(process_scrollbar_pointer_event(
        display,
        scrollbar_window,
        scrollbar));
    XDestroyImage(composited_before);
    composited_before = NULL;
    XDestroyImage(composited_after);
    composited_after = NULL;

    revision = state.revision;
    CHECK(test_compositor_set_owner(&compositor, false));
    CHECK(process_selection_change(display, transparency, 1));
    CHECK(boxrig_x11_transparency_context_get_state(transparency, &state) ==
          BOXRIG_UI_OK);
    CHECK(state.revision > revision);
    CHECK(state.effective == BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);
    CHECK(state.effective_background_opacity == 255);
    CHECK(boxrig_x11_renderer_apply_transparency_state(renderer, &state) == 0);
    CHECK(boxrig_x11_renderer_apply_transparency_state(renderer, &state) == 0);
    boxrig_x11_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    CHECK(boxrig_x11_renderer_get_render_statistics(renderer, &statistics) ==
          0);
    CHECK(statistics.row_raster_count == 1);
    CHECK(statistics.cell_raster_count == 8);
    raw = capture_window(display, terminal_window, width, height);
    CHECK(raw != NULL);
    CHECK((uint32_t)XGetPixel(
              raw,
              (int)(cell_width / 2),
              (int)(cell_height / 2)) ==
          boxrig_ui_argb32_premultiply(0x20, 0x40, 0x60, 255));
    XDestroyImage(raw);
    raw = NULL;
    boxrig_x11_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    CHECK(boxrig_x11_renderer_get_render_statistics(renderer, &statistics) ==
          0);
    CHECK(statistics.row_raster_count == 0);

    revision = state.revision;
    CHECK(test_compositor_set_owner(&compositor, true));
    CHECK(process_selection_change(display, transparency, 1));
    CHECK(boxrig_x11_transparency_context_get_state(transparency, &state) ==
          BOXRIG_UI_OK);
    CHECK(state.revision > revision);
    CHECK(state.effective == BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA);
    CHECK(state.effective_background_opacity == 128);
    CHECK(boxrig_x11_renderer_apply_transparency_state(renderer, &state) == 0);
    boxrig_x11_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    CHECK(boxrig_x11_renderer_get_render_statistics(renderer, &statistics) ==
          0);
    CHECK(statistics.row_raster_count == 1);
    CHECK(statistics.cell_raster_count == 8);
    boxrig_x11_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    CHECK(boxrig_x11_renderer_get_render_statistics(renderer, &statistics) ==
          0);
    CHECK(statistics.row_raster_count == 0);

    success = true;
    if (background_dirty != None) {
        XDamageDestroy(display, background_dirty);
    }
    boxrig_composition_destroy(composition);
    boxrig_x11_scrollbar_destroy(scrollbar);
    boxrig_terminal_destroy(terminal);
    boxrig_x11_renderer_destroy(renderer);
    if (terminal_window != None) {
        XDestroyWindow(display, terminal_window);
    }
    if (background != None) {
        XDestroyWindow(display, background);
    }
    boxrig_x11_transparency_context_destroy(transparency);
    test_compositor_destroy(&compositor);
    return success;
}

int main(void)
{
    Display *display = XOpenDisplay(NULL);

    if (display == NULL) {
        fprintf(stderr, "FAIL could not open X display\n");
        return 1;
    }
    if (!render_compositor_fixture(display)) {
        XCloseDisplay(display);
        cairo_debug_reset_static_data();
        FcFini();
        return 1;
    }
    XCloseDisplay(display);
    cairo_debug_reset_static_data();
    FcFini();
    printf("PASS X11 compositor preserves terminal background alpha\n");
    return 0;
}
