#define _POSIX_C_SOURCE 200809L

/* The guide uses public boxrig APIs and no private headers. */
#include "boxrig/terminal.h"
#include "boxrig/posix.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Draws only the cells in changed row intervals. */
static int draw_terminal(
    Display *display,
    Window window,
    GC graphics,
    XFontStruct *font,
    boxrig_terminal *terminal,
    unsigned int cell_width,
    unsigned int cell_height)
{
    boxrig_dirty dirty;

    XSetForeground(display, graphics, BlackPixel(display, DefaultScreen(display)));

    while (boxrig_terminal_take_dirty(terminal, &dirty)) {
        const size_t end = dirty.column + dirty.column_count;
        size_t column;

        for (column = dirty.column; column < end; column++) {
            boxrig_cell_view cell;
            char glyph;
            const int x = (int)(column * cell_width);
            const int y = (int)(dirty.row * cell_height);

            /* Finish using this view before the next dirty is consumed. */
            if (boxrig_terminal_get_cell(
                    terminal,
                    dirty.row,
                    column,
                    &cell) != BOXRIG_STATUS_OK) {
                /* Restore redraw work if drawing cannot be completed. */
                boxrig_terminal_invalidate(terminal);
                return -1;
            }

            /* Clear old glyphs even when the updated cell is empty. */
            XClearArea(display, window, x, y, cell_width, cell_height, False);

            /* This version still supports only one-byte ASCII cells. */
            if (cell.text_length != 1) {
                continue;
            }
            glyph = cell.text[0];
            XDrawString(
                display,
                window,
                graphics,
                x,
                y + font->ascent,
                &glyph,
                1);
        }
    }

    {
        size_t rows;
        size_t columns;
        boxrig_cursor_view cursor;

        boxrig_terminal_size(terminal, &rows, &columns);
        boxrig_terminal_get_cursor(terminal, &cursor);
        if (cursor.visible && cursor.row < rows && cursor.column < columns) {
            /* Draw the cursor after the cells so that their backgrounds do not erase it. */
            XDrawRectangle(
                display,
                window,
                graphics,
                (int)(cursor.column * cell_width),
                (int)(cursor.row * cell_height),
                cell_width - 1,
                cell_height - 1);
        }
    }

    XFlush(display);
    return 0;
}

/* Fits the terminal grid to the actual X11 window dimensions. */
static int resize_terminal(
    Display *display,
    Window window,
    boxrig_terminal *terminal,
    boxrig_posix_pty *pty,
    unsigned int cell_width,
    unsigned int cell_height,
    unsigned int width,
    unsigned int height)
{
    size_t rows;
    size_t columns;
    size_t current_rows;
    size_t current_columns;
    unsigned int grid_width;
    unsigned int grid_height;

    if (width == 0 || height == 0 || cell_width == 0 || cell_height == 0) {
        return -1;
    }

    /* Floor partial cells, keeping a minimum grid of one cell. */
    rows = height / cell_height;
    columns = width / cell_width;
    if (rows == 0) {
        rows = 1;
    }
    if (columns == 0) {
        columns = 1;
    }

    boxrig_terminal_size(terminal, &current_rows, &current_columns);
    if (rows != current_rows || columns != current_columns) {
        if (boxrig_terminal_resize(terminal, rows, columns) != BOXRIG_STATUS_OK ||
            boxrig_posix_pty_resize(pty, rows, columns) == -1) {
            return -1;
        }
    }

    /* Erase old glyphs in the strips outside the new complete-cell grid. */
    grid_width = (unsigned int)columns * cell_width;
    grid_height = (unsigned int)rows * cell_height;
    if (grid_width < width) {
        XClearArea(
            display, window, (int)grid_width, 0,
            width - grid_width, height, False);
    }
    if (grid_height < height) {
        XClearArea(
            display, window, 0, (int)grid_height,
            width, height - grid_height, False);
    }

    /* Pixel dimensions may change even when the cell count stays the same. */
    boxrig_terminal_invalidate(terminal);
    return 0;
}

/* Converts an X11 key press to one portable terminal input event. */
static boxrig_status send_key_press(
    boxrig_terminal *terminal,
    XKeyEvent *xevent)
{
    char text[32];
    KeySym keysym = NoSymbol;
    const int length = XLookupString(
        xevent, text, (int)sizeof(text), &keysym, NULL);
    boxrig_key_event event = {0};

    event.action = BOXRIG_KEY_PRESS;
    if (xevent->state & ShiftMask) {
        event.modifiers |= BOXRIG_MODIFIER_SHIFT;
    }
    if (xevent->state & ControlMask) {
        event.modifiers |= BOXRIG_MODIFIER_CONTROL;
    }
    /* This example assumes that Mod1 is assigned to Alt. */
    if (xevent->state & Mod1Mask) {
        event.modifiers |= BOXRIG_MODIFIER_ALT;
    }
    if (xevent->state & LockMask) {
        event.modifiers |= BOXRIG_MODIFIER_CAPS_LOCK;
    }

    switch (keysym) {
    case XK_Return:    event.key = BOXRIG_KEY_ENTER; break;
    case XK_BackSpace: event.key = BOXRIG_KEY_BACKSPACE; break;
    case XK_Tab:
    case XK_ISO_Left_Tab: event.key = BOXRIG_KEY_TAB; break;
    case XK_Escape:    event.key = BOXRIG_KEY_ESCAPE; break;
    case XK_Left:     event.key = BOXRIG_KEY_LEFT; break;
    case XK_Right:    event.key = BOXRIG_KEY_RIGHT; break;
    case XK_Up:       event.key = BOXRIG_KEY_UP; break;
    case XK_Down:     event.key = BOXRIG_KEY_DOWN; break;
    default: {
        const KeySym unshifted = XLookupKeysym(xevent, 0);
        const KeySym shifted = XLookupKeysym(xevent, 1);
        int index;

        /* Latin-1 bytes outside ASCII are not valid standalone UTF-8. */
        for (index = 0; index < length; index++) {
            if ((unsigned char)text[index] > 0x7f) {
                return BOXRIG_STATUS_OK;
            }
        }
        if (unshifted < 0x20 || unshifted > 0x7e) {
            return BOXRIG_STATUS_OK;
        }
        event.key = BOXRIG_KEY_NONE;
        event.codepoint = (uint32_t)unshifted;
        if (shifted >= 0x20 && shifted <= 0x7e) {
            event.shifted_codepoint = (uint32_t)shifted;
        }
        /* Keep the native text, including Control-generated ASCII bytes. */
        event.text = text;
        event.text_length = (size_t)length;
        break;
    }
    }

    /* The stack-owned text remains valid for this synchronous call. */
    return boxrig_terminal_send_key(terminal, &event);
}

/* Handles bell items without retaining any boxrig-owned view. */
static int process_host_items(
    Display *display,
    boxrig_terminal *terminal)
{
    boxrig_host_item item;

    while (boxrig_terminal_host_item_peek(terminal, &item)) {
        const uint64_t request_id = item.request_id;

        /* Handle bells and ignore unsupported host operations. */
        if (item.type == BOXRIG_HOST_ITEM_BELL) {
            (void)XBell(display, 0);
        }

        if (boxrig_terminal_host_item_consume(terminal) != BOXRIG_STATUS_OK) {
            return -1;
        }
        /* Resolve unsupported requests after consuming their items. */
        if (request_id != 0 &&
            boxrig_terminal_complete_host_request(
                terminal, request_id, BOXRIG_HOST_REQUEST_UNAVAILABLE,
                NULL, 0) != BOXRIG_STATUS_OK) {
            return -1;
        }
    }
    XFlush(display);
    return 0;
}

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_posix_pty *pty = NULL;
    boxrig_session *session = NULL;
    Display *display = NULL;
    int screen = 0;
    Window window = None;
    XFontStruct *font = NULL;
    GC graphics = NULL;
    Atom delete_window = None;
    unsigned int cell_width;
    unsigned int cell_height;
    bool running = true;
    int result = 1;

    /* X11 resources remain owned by this frontend, never by boxrig. */
    display = XOpenDisplay(NULL);
    if (display == NULL) {
        fprintf(stderr, "hello-boxrig: could not open X display\n");
        goto cleanup;
    }
    screen = DefaultScreen(display);
    font = XLoadQueryFont(display, "fixed");
    if (font == NULL || font->max_bounds.width <= 0 ||
        font->ascent <= 0 || font->descent < 0) {
        fprintf(stderr, "hello-boxrig: fixed font is unavailable\n");
        goto cleanup;
    }
    cell_width = (unsigned int)font->max_bounds.width;
    cell_height = (unsigned int)(font->ascent + font->descent);

    /* Configure the model before creating the matching pixel-sized window. */
    config.rows = 25;
    config.columns = 80;

    /* The child process supplies terminal input from this point on. */
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK) {
        fprintf(stderr, "hello-boxrig: could not create terminal\n");
        goto cleanup;
    }

    /* One fixed-font character cell maps to one terminal grid cell. */
    {
        size_t rows;
        size_t columns;

        /* Use the created terminal's dimensions for the initial window size. */
        boxrig_terminal_size(terminal, &rows, &columns);
        window = XCreateSimpleWindow(
            display,
            RootWindow(display, screen),
            0,
            0,
            (unsigned int)columns * cell_width,
            (unsigned int)rows * cell_height,
            0,
            BlackPixel(display, screen),
            WhitePixel(display, screen));
    }

    if (window == None) {
        goto cleanup;
    }
    graphics = XCreateGC(display, window, 0, NULL);
    if (graphics == NULL) {
        goto cleanup;
    }
    XSetFont(display, graphics, font->fid);
    XSelectInput(display, window, ExposureMask | KeyPressMask | StructureNotifyMask);
    delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    if (delete_window == None ||
        XSetWMProtocols(display, window, &delete_window, 1) == 0) {
        goto cleanup;
    }
    XStoreName(display, window, "boxrig core guide");
    XMapWindow(display, window);

    {
        char *arguments[] = { "/bin/sh", "-i", NULL };
        boxrig_session_config session_config = boxrig_session_config_default();
        size_t rows;
        size_t columns;

        boxrig_terminal_size(terminal, &rows, &columns);
        if (boxrig_posix_pty_spawn(arguments, "xterm", rows, columns, &pty) == -1 ||
            boxrig_posix_pty_session_create(
                terminal, pty, &session_config, &session) != BOXRIG_SESSION_OK) {
            fprintf(stderr, "hello-boxrig: could not connect child process\n");
            goto cleanup;
        }
    }

    uint32_t ready = BOXRIG_SESSION_READY_NONE;

    while (running) {
        XEvent event;
        boxrig_session_dispatch_result dispatched;
        boxrig_host_item item;
        uint32_t interests;
        bool had_host_items;
        bool retry_input;
        int timeout;
        struct pollfd descriptors[2] = {
            { XConnectionNumber(display), POLLIN, 0 },
            { boxrig_posix_pty_descriptor(pty), 0, 0 }
        };

        /* Retained input is retried even when ready is NONE. */
        if (boxrig_session_dispatch(session, ready, &dispatched) != BOXRIG_SESSION_OK) {
            fprintf(stderr, "hello-boxrig: could not exchange terminal data\n");
            goto cleanup;
        }
        had_host_items = boxrig_terminal_host_item_peek(terminal, &item);
        if (process_host_items(display, terminal) == -1 ||
            draw_terminal(
                display, window, graphics, font,
                terminal, cell_width, cell_height) == -1) {
            fprintf(stderr, "hello-boxrig: could not process terminal results\n");
            goto cleanup;
        }
        if (dispatched.peer_closed) {
            break;
        }

        interests = boxrig_session_interests(session);
        if ((interests & BOXRIG_SESSION_INTEREST_READ) != 0) {
            descriptors[1].events |= POLLIN;
        }
        if ((interests & BOXRIG_SESSION_INTEREST_WRITE) != 0) {
            descriptors[1].events |= POLLOUT;
        }

        /* Retry after capacity was freed; otherwise wait for PTY writability. */
        retry_input = (interests & BOXRIG_SESSION_INTEREST_DISPATCH) != 0 &&
            (!dispatched.core_blocked || had_host_items ||
             dispatched.transport_bytes_written != 0);
        timeout = XPending(display) > 0 || retry_input ? 0 : -1;
        ready = BOXRIG_SESSION_READY_NONE;
        if (poll(descriptors, 2, timeout) == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("hello-boxrig: poll");
            goto cleanup;
        }
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
            (descriptors[1].revents & POLLNVAL) != 0) {
            fprintf(stderr, "hello-boxrig: connection is unavailable\n");
            goto cleanup;
        }
        /* Read remaining bytes before treating a PTY hangup as EOF. */
        if ((descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            ready |= BOXRIG_SESSION_READY_READ;
        }
        if ((descriptors[1].revents & (POLLOUT | POLLHUP | POLLERR)) != 0) {
            ready |= BOXRIG_SESSION_READY_WRITE;
        }

        /* Xlib may already have queued events even when its descriptor is quiet. */
        if (XPending(display) == 0) {
            continue;
        }
        XNextEvent(display, &event);
        if (event.type == Expose && event.xexpose.count == 0) {
            /* Expose requires repainting even when terminal cells have not changed. */
            boxrig_terminal_invalidate(terminal);
            if (draw_terminal(
                    display,
                    window,
                    graphics,
                    font,
                    terminal,
                    cell_width,
                    cell_height) == -1) {
                goto cleanup;
            }
        } else if (event.type == KeyPress) {
            const boxrig_status status =
                send_key_press(terminal, &event.xkey);

            if (status != BOXRIG_STATUS_OK) {
                fprintf(stderr, "hello-boxrig: could not send key (%d)\n", (int)status);
                goto cleanup;
            }
        } else if (event.type == MappingNotify) {
            /* Keep Xlib's cached keymap up to date. */
            XRefreshKeyboardMapping(&event.xmapping);
        } else if (event.type == ConfigureNotify &&
                 event.xconfigure.window == window &&
                 event.xconfigure.width > 0 && event.xconfigure.height > 0) {
            if (resize_terminal(
                    display,
                    window,
                    terminal,
                    pty,
                    cell_width,
                    cell_height,
                    (unsigned int)event.xconfigure.width,
                    (unsigned int)event.xconfigure.height) == -1 ||
                draw_terminal(
                    display,
                    window,
                    graphics,
                    font,
                    terminal,
                    cell_width,
                    cell_height) == -1) {
                fprintf(stderr, "hello-boxrig: could not resize terminal\n");
                goto cleanup;
            }
        } else if (event.type == ClientMessage &&
                   (Atom)event.xclient.data.l[0] == delete_window) {
            running = false;
        }
    }
    result = 0;

cleanup:
    /* Release the borrower before its terminal and PTY. */
    boxrig_session_destroy(session);
    if (pty != NULL) {
        int child_status;

        /* Stop the direct child before waiting for its termination. */
        (void)kill(boxrig_posix_pty_child(pty), SIGKILL);
        if (boxrig_posix_pty_wait(pty, &child_status) == -1) {
            perror("hello-boxrig: could not reap child process");
            result = 1;
        }
        boxrig_posix_pty_destroy(pty);
    }

    /* No cell view survives here; release each owned resource exactly once. */
    if (display != NULL && graphics != NULL) {
        XFreeGC(display, graphics);
    }
    if (display != NULL && window != None) {
        XDestroyWindow(display, window);
    }
    if (display != NULL && font != NULL) {
        XFreeFont(display, font);
    }
    if (display != NULL) {
        XCloseDisplay(display);
    }
    boxrig_terminal_destroy(terminal);
    return result;
}
