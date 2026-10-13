/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/x11/scrollbar.h"

#include "boxrig/ui/scrollbar.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BOXRIG_X11_SCROLLBAR_WIDTH 14U
#define BOXRIG_X11_SCROLLBAR_MINIMUM_THUMB 14U

struct boxrig_x11_scrollbar {
    Display *display;
    Window parent;
    Window window;
    boxrig_terminal *terminal;
    GC graphics;
    Colormap colormap;
    unsigned long track_pixel;
    unsigned long thumb_pixel;
    unsigned long disabled_pixel;
    bool track_allocated;
    bool thumb_allocated;
    bool disabled_allocated;
    int x;
    unsigned int width;
    unsigned int height;
    boxrig_scrollbar_model model;
    boxrig_scrollbar_thumb thumb;
    bool drawn;
    bool visible;
    bool dragging;
    int drag_anchor;
};

static unsigned long allocate_color(
    Display *display,
    Colormap colormap,
    unsigned short component,
    unsigned long fallback,
    bool *allocated_out)
{
    XColor color;

    memset(&color, 0, sizeof(color));
    color.red = component;
    color.green = component;
    color.blue = component;
    color.flags = DoRed | DoGreen | DoBlue;
    if (XAllocColor(display, colormap, &color) == 0) {
        *allocated_out = false;
        return fallback;
    }
    *allocated_out = true;
    return color.pixel;
}

static bool models_equal(
    const boxrig_scrollbar_model *left,
    const boxrig_scrollbar_model *right)
{
    return left->history_rows == right->history_rows &&
           left->viewport_rows == right->viewport_rows &&
           left->viewport_offset == right->viewport_offset &&
           left->position == right->position &&
           left->enabled == right->enabled;
}

static int draw_scrollbar(
    boxrig_x11_scrollbar *scrollbar,
    bool force)
{
    boxrig_scrollbar_model model;
    boxrig_scrollbar_thumb thumb;
    unsigned long thumb_pixel;

    if (scrollbar == NULL || scrollbar->width == 0 ||
        scrollbar->height == 0 ||
        boxrig_scrollbar_model_read(scrollbar->terminal, &model) == -1) {
        return -1;
    }
    boxrig_scrollbar_calculate_thumb(
        &model,
        scrollbar->height,
        BOXRIG_X11_SCROLLBAR_MINIMUM_THUMB,
        &thumb);
    if (!force && scrollbar->drawn &&
        models_equal(&scrollbar->model, &model) &&
        scrollbar->thumb.start == thumb.start &&
        scrollbar->thumb.length == thumb.length) {
        return 0;
    }
    XSetForeground(
        scrollbar->display,
        scrollbar->graphics,
        scrollbar->track_pixel);
    XFillRectangle(
        scrollbar->display,
        scrollbar->window,
        scrollbar->graphics,
        0,
        0,
        scrollbar->width,
        scrollbar->height);
    thumb_pixel = model.enabled
                      ? scrollbar->thumb_pixel
                      : scrollbar->disabled_pixel;
    XSetForeground(
        scrollbar->display,
        scrollbar->graphics,
        thumb_pixel);
    if (thumb.length != 0) {
        XFillRectangle(
            scrollbar->display,
            scrollbar->window,
            scrollbar->graphics,
            0,
            (int)thumb.start,
            scrollbar->width,
            (unsigned int)thumb.length);
    }
    scrollbar->model = model;
    scrollbar->thumb = thumb;
    scrollbar->drawn = true;
    return 0;
}

static int apply_action(
    boxrig_x11_scrollbar *scrollbar,
    boxrig_scrollbar_action action,
    size_t position)
{
    bool changed;

    if (boxrig_scrollbar_apply_action(
            scrollbar->terminal,
            action,
            position,
            &changed) == -1) {
        return -1;
    }
    return draw_scrollbar(scrollbar, changed);
}

unsigned int boxrig_x11_scrollbar_pixel_width(void)
{
    return BOXRIG_X11_SCROLLBAR_WIDTH;
}

int boxrig_x11_scrollbar_create(
    Display *display,
    Window parent,
    boxrig_terminal *terminal,
    boxrig_x11_scrollbar **scrollbar_out)
{
    boxrig_x11_scrollbar *scrollbar;
    int screen;
    unsigned long black;
    unsigned long white;
    XSetWindowAttributes attributes;

    if (display == NULL || parent == None || terminal == NULL ||
        scrollbar_out == NULL) {
        return -1;
    }
    *scrollbar_out = NULL;
    scrollbar = calloc(1, sizeof(*scrollbar));
    if (scrollbar == NULL) {
        return -1;
    }
    scrollbar->display = display;
    scrollbar->parent = parent;
    scrollbar->terminal = terminal;
    scrollbar->width = 1;
    scrollbar->height = 1;
    screen = DefaultScreen(display);
    black = BlackPixel(display, screen);
    white = WhitePixel(display, screen);
    scrollbar->colormap = DefaultColormap(display, screen);
    scrollbar->track_pixel = allocate_color(
        display,
        scrollbar->colormap,
        0x2424,
        black,
        &scrollbar->track_allocated);
    scrollbar->thumb_pixel = allocate_color(
        display,
        scrollbar->colormap,
        0x9090,
        white,
        &scrollbar->thumb_allocated);
    scrollbar->disabled_pixel = allocate_color(
        display,
        scrollbar->colormap,
        0x4848,
        white,
        &scrollbar->disabled_allocated);
    memset(&attributes, 0, sizeof(attributes));
    attributes.background_pixel = scrollbar->track_pixel;
    attributes.border_pixel = scrollbar->track_pixel;
    attributes.colormap = scrollbar->colormap;
    scrollbar->window = XCreateWindow(
        display,
        parent,
        0,
        0,
        1,
        1,
        0,
        DefaultDepth(display, screen),
        InputOutput,
        DefaultVisual(display, screen),
        CWBackPixel | CWBorderPixel | CWColormap,
        &attributes);
    if (scrollbar->window == None) {
        boxrig_x11_scrollbar_destroy(scrollbar);
        return -1;
    }
    scrollbar->graphics = XCreateGC(
        display,
        scrollbar->window,
        0,
        NULL);
    if (scrollbar->graphics == NULL ||
        XStoreName(display, scrollbar->window, "boxrig-scrollbar") == 0) {
        boxrig_x11_scrollbar_destroy(scrollbar);
        return -1;
    }
    XSelectInput(
        display,
        scrollbar->window,
        ExposureMask | ButtonPressMask | ButtonReleaseMask |
            PointerMotionMask);
    XMapWindow(display, scrollbar->window);
    scrollbar->visible = true;
    *scrollbar_out = scrollbar;
    return 0;
}

Window boxrig_x11_scrollbar_window(const boxrig_x11_scrollbar *scrollbar)
{
    return scrollbar == NULL ? None : scrollbar->window;
}

void boxrig_x11_scrollbar_set_visible(boxrig_x11_scrollbar *scrollbar, bool visible)
{
    if (scrollbar == NULL || scrollbar->visible == visible) return;
    scrollbar->visible = visible;
    scrollbar->drawn = false;
    if (visible) {
        XMapWindow(scrollbar->display, scrollbar->window);
    } else {
        scrollbar->dragging = false;
        XUnmapWindow(scrollbar->display, scrollbar->window);
    }
}

void boxrig_x11_scrollbar_destroy(boxrig_x11_scrollbar *scrollbar)
{
    unsigned long pixel;

    if (scrollbar == NULL) {
        return;
    }
    if (scrollbar->graphics != NULL) {
        XFreeGC(scrollbar->display, scrollbar->graphics);
    }
    if (scrollbar->window != None) {
        XDestroyWindow(scrollbar->display, scrollbar->window);
    }
    if (scrollbar->track_allocated) {
        pixel = scrollbar->track_pixel;
        XFreeColors(scrollbar->display, scrollbar->colormap, &pixel, 1, 0);
    }
    if (scrollbar->thumb_allocated) {
        pixel = scrollbar->thumb_pixel;
        XFreeColors(scrollbar->display, scrollbar->colormap, &pixel, 1, 0);
    }
    if (scrollbar->disabled_allocated) {
        pixel = scrollbar->disabled_pixel;
        XFreeColors(scrollbar->display, scrollbar->colormap, &pixel, 1, 0);
    }
    free(scrollbar);
}

int boxrig_x11_scrollbar_resize(
    boxrig_x11_scrollbar *scrollbar,
    unsigned int parent_width,
    unsigned int parent_height)
{
    unsigned int width;
    unsigned int height;
    int x;

    if (scrollbar == NULL || parent_width == 0 || parent_height == 0) {
        return -1;
    }
    width = parent_width < BOXRIG_X11_SCROLLBAR_WIDTH
                ? parent_width
                : BOXRIG_X11_SCROLLBAR_WIDTH;
    height = parent_height;
    x = (int)(parent_width - width);
    if (scrollbar->x == x && scrollbar->width == width &&
        scrollbar->height == height) {
        return 0;
    }
    scrollbar->x = x;
    scrollbar->width = width;
    scrollbar->height = height;
    scrollbar->drawn = false;
    XMoveResizeWindow(
        scrollbar->display,
        scrollbar->window,
        x,
        0,
        width,
        height);
    return 0;
}

int boxrig_x11_scrollbar_synchronize(boxrig_x11_scrollbar *scrollbar)
{
    return draw_scrollbar(scrollbar, false);
}

int boxrig_x11_scrollbar_process_event(
    boxrig_x11_scrollbar *scrollbar,
    const XEvent *event)
{
    if (scrollbar == NULL || event == NULL) {
        return -1;
    }
    if (!scrollbar->visible) return 0;
    if (event->type == Expose &&
        event->xexpose.window == scrollbar->window) {
        return draw_scrollbar(scrollbar, true) == -1 ? -1 : 1;
    }
    if (event->type == ButtonPress &&
        event->xbutton.window == scrollbar->window) {
        int y = event->xbutton.y;

        if (draw_scrollbar(scrollbar, false) == -1) {
            return -1;
        }
        if (event->xbutton.button == Button1 && scrollbar->model.enabled) {
            size_t clamped_y = y < 0 ? 0 : (size_t)y;

            if (clamped_y >= scrollbar->thumb.start &&
                clamped_y < scrollbar->thumb.start +
                                scrollbar->thumb.length) {
                scrollbar->dragging = true;
                scrollbar->drag_anchor =
                    y - (int)scrollbar->thumb.start;
            } else if (clamped_y < scrollbar->thumb.start) {
                if (apply_action(
                        scrollbar,
                        BOXRIG_SCROLLBAR_PAGE_UP,
                        0) == -1) {
                    return -1;
                }
            } else if (apply_action(
                           scrollbar,
                           BOXRIG_SCROLLBAR_PAGE_DOWN,
                           0) == -1) {
                return -1;
            }
        } else if (event->xbutton.button == Button4) {
            if (apply_action(
                    scrollbar,
                    BOXRIG_SCROLLBAR_LINE_UP,
                    0) == -1) {
                return -1;
            }
        } else if (event->xbutton.button == Button5 &&
                   apply_action(
                       scrollbar,
                       BOXRIG_SCROLLBAR_LINE_DOWN,
                       0) == -1) {
            return -1;
        }
        return 1;
    }
    if (event->type == MotionNotify &&
        event->xmotion.window == scrollbar->window) {
        if (scrollbar->dragging) {
            int64_t requested =
                (int64_t)event->xmotion.y - scrollbar->drag_anchor;
            size_t thumb_start = requested < 0 ? 0 : (size_t)requested;
            size_t position;

            if (draw_scrollbar(scrollbar, false) == -1) {
                return -1;
            }
            position = boxrig_scrollbar_position_from_thumb(
                &scrollbar->model,
                scrollbar->height,
                BOXRIG_X11_SCROLLBAR_MINIMUM_THUMB,
                thumb_start);
            if (apply_action(
                    scrollbar,
                    BOXRIG_SCROLLBAR_POSITION,
                    position) == -1) {
                return -1;
            }
        }
        return 1;
    }
    if (event->type == ButtonRelease &&
        event->xbutton.window == scrollbar->window) {
        if (event->xbutton.button == Button1) {
            scrollbar->dragging = false;
        }
        return 1;
    }
    return 0;
}
