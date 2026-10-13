/* Observe the server's current native cursor without moving the pointer. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/extensions/Xfixes.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double cursor_pixel_match(Display *display, const char *theme,
    const char *name, int x, int y, int size)
{
    XcursorImage *cursor = XcursorLibraryLoadImage(name, theme, size);
    if (cursor == NULL) return 0;
    if (x < (int)cursor->xhot || y < (int)cursor->yhot ||
        x - (int)cursor->xhot + (int)cursor->width > DisplayWidth(display, DefaultScreen(display)) ||
        y - (int)cursor->yhot + (int)cursor->height > DisplayHeight(display, DefaultScreen(display))) {
        XcursorImageDestroy(cursor);
        return 0;
    }
    XImage *screen = XGetImage(display, DefaultRootWindow(display),
        x - (int)cursor->xhot, y - (int)cursor->yhot,
        cursor->width, cursor->height, AllPlanes, ZPixmap);
    if (screen == NULL) { XcursorImageDestroy(cursor); return 0; }
    unsigned opaque = 0, matched = 0;
    for (unsigned row = 0; row < cursor->height; row++) {
        for (unsigned col = 0; col < cursor->width; col++) {
            uint32_t pixel = cursor->pixels[row * cursor->width + col];
            if ((pixel >> 24) != 255) continue;
            opaque++;
            if ((XGetPixel(screen, (int)col, (int)row) & 0xffffff) ==
                (pixel & 0xffffff)) matched++;
        }
    }
    XDestroyImage(screen);
    XcursorImageDestroy(cursor);
    return opaque == 0 ? 0 : (double)matched / opaque;
}

int main(int argc, char **argv)
{
    Display *display = XOpenDisplay(NULL);
    if (display == NULL) return 1;
    if (argc == 2 && strcmp(argv[1], "--pixels") == 0) {
        Window root, child;
        int x, y, local_x, local_y;
        unsigned mask;
        if (!XQueryPointer(display, DefaultRootWindow(display), &root, &child,
                &x, &y, &local_x, &local_y, &mask)) return 1;
        /* Nested Weston draws the Wayland pointer into the X11 framebuffer.
         * Compare the opaque theme pixels, independent of terminal content. */
        /* GDK uses the desktop cursor-size preference rather than the
         * XCURSOR_SIZE environment variable. Accept an exact theme match
         * at its supported sizes without changing that user preference. */
        const int sizes[] = {24, 32, 48, 64, 96, 128};
        const char *theme = getenv("BOXRIG_TEST_LINK_CURSOR_THEME");
        if (theme == NULL) theme = getenv("XCURSOR_THEME");
        double hand = 0, text = 0;
        int matched_size = 0;
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            double h = cursor_pixel_match(display, theme, "hand2", x, y, sizes[i]);
            double t = cursor_pixel_match(display, theme, "xterm", x, y, sizes[i]);
            if (h > hand) hand = h;
            if (t > text) text = t;
            if (h == 1 || t == 1) { matched_size = sizes[i]; break; }
        }
        printf("{\"name\":\"%s\",\"size\":%d,\"handMatch\":%.6f,\"textMatch\":%.6f}\n",
            hand == 1 ? "hand2" : text == 1 ? "xterm" : "", matched_size, hand, text);
        XCloseDisplay(display);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--focus-peer") == 0) {
        Window window = XCreateSimpleWindow(display, DefaultRootWindow(display),
            1650, 40, 120, 80, 0, 0, 0);
        XStoreName(display, window, "boxrig-link-focus-peer");
        XSelectInput(display, window, StructureNotifyMask);
        XMapWindow(display, window);
        XSync(display, False);
        printf("%lu\n", window);
        fflush(stdout);
        for (;;) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == DestroyNotify) break;
        }
        XCloseDisplay(display);
        return 0;
    }
    XFixesCursorImage *cursor = XFixesGetCursorImage(display);
    if (cursor == NULL) { XCloseDisplay(display); return 1; }
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < (size_t)cursor->width * cursor->height; i++) {
        hash ^= (uint32_t)cursor->pixels[i];
        hash *= UINT64_C(1099511628211);
    }
    printf("{\"name\":\"%s\",\"width\":%u,\"height\":%u,\"hash\":\"%016" PRIx64 "\"}\n",
        cursor->name == NULL ? "" : cursor->name,
        cursor->width, cursor->height, hash);
    XFree(cursor);
    XCloseDisplay(display);
    return 0;
}
