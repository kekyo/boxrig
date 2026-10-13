/* boxrig - Under MIT. A separate animated window for compositor validation. */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <assert.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--terminal") == 0) {
        fputs("\033]0;plan27-background-ready\007", stdout);
        fflush(stdout);
        char bytes[32];
        while (read(STDIN_FILENO, bytes, sizeof(bytes)) > 0) {}
        return 0;
    }
    Display *display = XOpenDisplay(NULL);
    assert(display != NULL);
    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1280, 900, 0, 0, 0);
    /* A desktop window remains below managed terminals in the compositor's
     * stacking order, including when a terminal is moved or resized. */
    Atom type = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
    Atom desktop = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    XChangeProperty(display, window, type, XA_ATOM, 32, PropModeReplace,
        (const unsigned char *)&desktop, 1);
    XStoreName(display, window, "plan27-backdrop");
    XMapWindow(display, window);
    XLowerWindow(display, window);
    GC gc = XCreateGC(display, window, 0, NULL);
    assert(gc != NULL);
    unsigned int frame = 0;
    for (;;) {
        for (int y = 0; y < 900; y += 96)
            for (int x = 0; x < 1280; x += 8) {
                bool bright = (((unsigned int)x / 8 + frame / 5) & 1) != 0;
                unsigned long color = bright ? 0xf0f0f0 : 0x181818;
                if (((unsigned int)y / 96 + frame / 25) & 1)
                    color = bright ? 0xe06020 : 0x2060e0;
                /* A global green change survives any blur radius; red edges
                 * independently measure the compositor's spatial filtering. */
                color = (color & 0xff00ffu) | ((frame / 25 & 1) ? 0xd000u : 0x5000u);
                XSetForeground(display, gc, color);
                XFillRectangle(display, window, gc, x, y, 8, 96);
            }
        XSync(display, False);
        /* This is the backdrop's animation clock, not a test success timeout. */
        struct pollfd input = {ConnectionNumber(display), POLLIN, 0};
        (void)poll(&input, 1, 40);
        while (XPending(display)) { XEvent event; XNextEvent(display, &event); }
        frame++;
    }
}
