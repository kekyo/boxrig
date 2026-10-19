/* boxrig - Under MIT. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
#define FRAME_LIMIT 2400

typedef struct frame {
    XImage *image;
    uint64_t start, end;
    size_t missed, source;
    unsigned long marker_pixel, window_pixel;
    int window_x, window_y;
    int marker_row;
    unsigned int selection_mask;
} frame;

typedef struct operation {
    const char *kind;
    uint64_t sent, seen, copied;
    unsigned int first_column, mask, attempts;
    char text[64];
} operation;

static uint64_t now_ns(clockid_t clock)
{
    struct timespec time;
    if (clock_gettime(clock, &time) != 0) abort();
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
}

static bool key(Display *display, KeySym symbol)
{
    KeyCode code = XKeysymToKeycode(display, symbol);
    if (code == 0 || !XTestFakeKeyEvent(display, code, True, CurrentTime) ||
        !XTestFakeKeyEvent(display, code, False, CurrentTime)) return false;
    XFlush(display);
    return true;
}

static bool write_ppm(const char *path, XImage *image)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    unsigned char *row = malloc((size_t)image->width * 3);
    if (row == NULL) { fclose(file); return false; }
    bool ok = fprintf(file, "P6\n%d %d\n255\n", image->width, image->height) > 0;
    for (int y = 0; y < image->height && ok; y++) {
        for (int x = 0; x < image->width; x++) {
            unsigned long pixel = XGetPixel(image, x, y);
            row[x * 3] = (unsigned char)(pixel >> 16);
            row[x * 3 + 1] = (unsigned char)(pixel >> 8);
            row[x * 3 + 2] = (unsigned char)pixel;
        }
        ok = fwrite(row, 3, (size_t)image->width, file) == (size_t)image->width;
    }
    free(row);
    return fclose(file) == 0 && ok;
}

int main(int argc, char **argv)
{
    REQUIRE(argc == 4);
    Window window = (Window)strtoul(argv[1], NULL, 0);
    bool record = strcmp(argv[3], "video") == 0;
    bool capture_root = getenv("BOXRIG_CAPTURE_ROOT") != NULL;
    Display *display = XOpenDisplay(NULL);
    REQUIRE(display != NULL);
    Window root = DefaultRootWindow(display), child;
    int event, error, major, minor, root_x, root_y;
    REQUIRE(XTestQueryExtension(display, &event, &error, &major, &minor));
    XWindowAttributes attributes;
    REQUIRE(XGetWindowAttributes(display, window, &attributes));
    REQUIRE(XTranslateCoordinates(display, window, root, 0, 0, &root_x, &root_y, &child));
    XSetInputFocus(display, window, RevertToParent, CurrentTime);
    XSync(display, False);

    /* Use markers on rows 3 and 8 to measure cell pitch. A top-row marker
     * can be partially obscured by window chrome and is not a height ruler. */
    int grid_x = -1, grid_y = -1, cw = 0, ch = 0;
    uint64_t deadline = now_ns(CLOCK_MONOTONIC) + UINT64_C(10000000000);
    while (now_ns(CLOCK_MONOTONIC) < deadline && cw == 0) {
        XImage *image = XGetImage(display, root, root_x, root_y,
            (unsigned int)attributes.width, (unsigned int)attributes.height, AllPlanes, ZPixmap);
        REQUIRE(image != NULL && image->red_mask == 0xff0000 &&
            image->green_mask == 0xff00 && image->blue_mask == 0xff);
        for (int y = 0; y < image->height && cw == 0; y++) {
            for (int x = 0; x < image->width && cw == 0; x++) {
                if ((XGetPixel(image, x, y) & 0xffffff) != 0x406020) continue;
                int width = 0, height = 0;
                while (x + width < image->width &&
                    (XGetPixel(image, x + width, y) & 0xffffff) == 0x406020) width++;
                while (y + height < image->height &&
                    (XGetPixel(image, x, y + height) & 0xffffff) == 0x406020) height++;
                if (width >= 40 && width % 8 == 0 && height >= 8) {
                    int second = y + height;
                    while (second < image->height &&
                        (XGetPixel(image, x, second) & 0xffffff) != 0x406020) second++;
                    if (second < image->height && (second - y) % 5 == 0) {
                        ch = (second - y) / 5;
                        grid_x = root_x + x; grid_y = root_y + y - 2 * ch; cw = width / 8;
                    }
                }
            }
        }
        XDestroyImage(image);
        if (cw == 0) (void)poll(NULL, 0, 10);
    }
    REQUIRE(cw > 0 && ch > 0);
    REQUIRE(grid_x >= 1222 && grid_y >= 0 && grid_x + 80 * cw <= 3142 && grid_y + 24 * ch <= 1080);
    XImage *last_row = XGetImage(display, root, grid_x + 3 * cw,
        grid_y + 23 * ch + ch / 2, 1, 1, AllPlanes, ZPixmap);
    REQUIRE(last_row != NULL);
    unsigned long last_pixel = XGetPixel(last_row, 0, 0) & 0xffffff;
    XDestroyImage(last_row);
    REQUIRE(last_pixel == 0x604020);
    frame *frames = calloc(FRAME_LIMIT, sizeof(*frames));
    REQUIRE(frames != NULL);
    int timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    struct itimerspec period = {{0, 16666667}, {0, 1}};
    REQUIRE(timer >= 0 && timerfd_settime(timer, 0, &period, NULL) == 0);
    uint64_t sent[20] = {0}, seen[20] = {0};
    size_t frame_count = 0, key_count = 0, seen_count = 0;
    unsigned long unselected_pixels[16] = {0};
    operation operations[20] = {{0}};
    size_t operation_count = 0;
    uint64_t key_phase_end = 0, next_operation = 0;
    bool selection_pending = false;
    Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
    Atom property = XInternAtom(display, "BOXRIG_INTERACTIVE_SELECTION", False);
    Window requestor = XCreateSimpleWindow(display, root, 0, 0, 1, 1, 0, 0, 0);
    REQUIRE(utf8 != None && property != None && requestor != None);
    uint64_t started = now_ns(CLOCK_MONOTONIC), cpu_start = now_ns(CLOCK_PROCESS_CPUTIME_ID);
    REQUIRE(key(display, XK_g));
    while (true) {
        struct pollfd fd = {timer, POLLIN, 0};
        REQUIRE(poll(&fd, 1, 5000) == 1);
        uint64_t expirations;
        REQUIRE(read(timer, &expirations, sizeof(expirations)) == sizeof(expirations));
        uint64_t now = now_ns(CLOCK_MONOTONIC);
        if (now - started >= UINT64_C(30000000000)) {
            if (frame_count != 0) {
                char diagnostic[4096];
                snprintf(diagnostic, sizeof(diagnostic), "%s/timeout.ppm", argv[2]);
                (void)write_ppm(diagnostic, frames[frame_count - 1].image);
                fprintf(stderr, "operation=%zu marker_row=%d mask=%x text=%s\n",
                    operation_count, frames[frame_count - 1].marker_row,
                    frames[frame_count - 1].selection_mask,
                    operation_count < 20 ? operations[operation_count].text : "complete");
            }
            REQUIRE(false);
        }
        bool keys_done = key_count == 20 && (!record || seen_count == 20) &&
            now >= sent[19] + UINT64_C(250000000);
        if (keys_done && !record) break;
        if (keys_done && key_phase_end == 0) {
            key_phase_end = now;
            next_operation = now;
        }
        if (operation_count == 20 && now >= next_operation) break;
        if (key_count < 20 && (!record || seen_count == key_count) &&
            now >= started + UINT64_C(500000000) + key_count * UINT64_C(250000000)) {
            sent[key_count++] = now_ns(CLOCK_MONOTONIC);
            REQUIRE(key(display, XK_k));
        }
        if (!record) continue;
        operation *active = operation_count < 20 ? &operations[operation_count] : NULL;
        if (active != NULL && key_phase_end != 0 && now >= next_operation && active->sent == 0) {
            active->sent = now_ns(CLOCK_MONOTONIC);
            if (operation_count < 10) {
                bool up = operation_count % 2 == 0;
                active->kind = up ? "up" : "down";
                REQUIRE(XTestFakeMotionEvent(display, -1, grid_x + 40 * cw, grid_y + 12 * ch, CurrentTime));
                REQUIRE(XTestFakeButtonEvent(display, up ? 4 : 5, True, CurrentTime));
                REQUIRE(XTestFakeButtonEvent(display, up ? 4 : 5, False, CurrentTime));
            } else {
                bool select = operation_count % 2 == 0;
                active->kind = select ? "select" : "clear";
                active->first_column = (operation_count / 2) % 2 == 1 ? 8 : 15;
                active->mask = select ? (active->first_column == 8 ? 0xffff : 0xff80) : 0;
                int x = grid_x + (select ? (int)active->first_column : 40) * cw + 1;
                REQUIRE(XTestFakeMotionEvent(display, -1, x, grid_y + ch / 2, CurrentTime));
                REQUIRE(XTestFakeButtonEvent(display, 1, True, CurrentTime));
                if (select) REQUIRE(XTestFakeMotionEvent(display, -1,
                    grid_x + 24 * cw - 2, grid_y + ch / 2, CurrentTime));
                REQUIRE(XTestFakeButtonEvent(display, 1, False, CurrentTime));
            }
            XFlush(display);
        }
        while (XPending(display) != 0) {
            XEvent reply;
            XNextEvent(display, &reply);
            if (reply.type != SelectionNotify || reply.xselection.requestor != requestor) continue;
            REQUIRE(active != NULL && selection_pending);
            selection_pending = false;
            if (reply.xselection.property == None) continue;
            Atom type;
            int format;
            unsigned long length, remaining;
            unsigned char *data = NULL;
            REQUIRE(XGetWindowProperty(display, requestor, property, 0, 16, True,
                utf8, &type, &format, &length, &remaining, &data) == Success);
            REQUIRE(type == utf8 && format == 8 && remaining == 0 && length < sizeof(active->text));
            memcpy(active->text, data, length);
            active->text[length] = '\0';
            XFree(data);
            const char *expected = active->first_column == 8 ? "SELECT_THIS_TEXT" : "THIS_TEXT";
            if (strcmp(active->text, expected) == 0) active->copied = now_ns(CLOCK_MONOTONIC);
        }
        if (active != NULL && active->sent != 0 && strcmp(active->kind, "select") == 0 &&
            active->copied == 0 && !selection_pending) {
            XConvertSelection(display, XA_PRIMARY, utf8, property, requestor, CurrentTime);
            XFlush(display);
            selection_pending = true;
            active->attempts++;
        }
        REQUIRE(frame_count < FRAME_LIMIT);
        frame *current = &frames[frame_count];
        current->start = now_ns(CLOCK_MONOTONIC);
        /* Redirected window pixels avoid sampling an intermediate desktop
         * composition. Root capture remains available for acquisition audits. */
        current->image = XGetImage(display, capture_root ? root : window,
            capture_root ? grid_x : grid_x - root_x,
            capture_root ? grid_y : grid_y - root_y,
            (unsigned int)(80 * cw), (unsigned int)(24 * ch), AllPlanes, ZPixmap);
        current->end = now_ns(CLOCK_MONOTONIC);
        current->missed = (size_t)(expirations - 1);
        REQUIRE(current->image != NULL);
        current->marker_pixel = XGetPixel(current->image, cw * 3, ch / 2) & 0xffffff;
        current->window_pixel = current->marker_pixel;
        current->window_x = root_x;
        current->window_y = root_y;
        unsigned long red = current->marker_pixel >> 16;
        if (key_phase_end == 0 && current->marker_pixel != 0x204060 &&
            !((current->marker_pixel & 0xffff) == 0xa050 && red >= 69 && red <= 164 && (red - 64) % 5 == 0)) {
            REQUIRE(XTranslateCoordinates(display, window, root, 0, 0,
                &current->window_x, &current->window_y, &child));
            XImage *diagnostic = XGetImage(display, window,
                grid_x - root_x + cw * 3, grid_y - root_y + ch / 2,
                1, 1, AllPlanes, ZPixmap);
            REQUIRE(diagnostic != NULL);
            current->window_pixel = XGetPixel(diagnostic, 0, 0) & 0xffffff;
            XDestroyImage(diagnostic);
        }
        current->marker_row = -1;
        for (int row = 0; row < 24; row++) {
            if ((XGetPixel(current->image, 3 * cw, row * ch + ch / 2) & 0xffffff) == 0xa4a050) {
                current->marker_row = row;
                break;
            }
        }
        for (int column = 8; column < 24; column++) {
            /* Both outline and filled selection change the bottom cell edge.
             * Compare against actual glyph ink, not an assumed black background. */
            unsigned long pixel = XGetPixel(current->image, (column + 1) * cw - 1, ch - 1) & 0xffffff;
            if (frame_count == 0) unselected_pixels[column - 8] = pixel;
            if (pixel != unselected_pixels[column - 8])
                current->selection_mask |= 1u << (column - 8);
        }
        if (key_phase_end == 0) REQUIRE(current->selection_mask == 0);
        if (key_count > seen_count &&
            current->marker_pixel ==
                ((64 + key_count * 5) << 16 | 160 << 8 | 80)) {
            seen[seen_count++] = current->end;
        }
        if (active != NULL && active->sent != 0) {
            bool visible = operation_count < 10
                ? (operation_count % 2 == 0 ? current->marker_row > 0 : current->marker_row == 0)
                : current->marker_row == 0 && current->selection_mask == active->mask;
            if (visible && active->seen == 0) active->seen = current->end;
            if (active->seen != 0 && (strcmp(active->kind, "select") != 0 || active->copied != 0)) {
                operation_count++;
                next_operation = current->end + UINT64_C(250000000);
            }
        }
        current->source = frame_count;
        if (frame_count != 0) {
            frame *previous = &frames[frame_count - 1];
            if (memcmp(current->image->data, previous->image->data,
                    (size_t)current->image->bytes_per_line * current->image->height) == 0) {
                XDestroyImage(current->image);
                current->image = previous->image;
                current->source = previous->source;
            }
        }
        frame_count++;
    }
    uint64_t ended = now_ns(CLOCK_MONOTONIC), cpu_end = now_ns(CLOCK_PROCESS_CPUTIME_ID);
    REQUIRE(key(display, XK_q));
    XSync(display, False);
    close(timer);
    char path[4096];
    REQUIRE(snprintf(path, sizeof(path), "%s/events.tsv", argv[2]) < (int)sizeof(path));
    FILE *events = fopen(path, "w");
    REQUIRE(events != NULL);
    fprintf(events, "# grid_x=%d grid_y=%d cw=%d ch=%d start_ns=%llu end_ns=%llu cpu_ns=%llu record=%d capture_root=%d key_phase_end_ns=%llu\n",
        grid_x, grid_y, cw, ch, (unsigned long long)started, (unsigned long long)ended,
        (unsigned long long)(cpu_end - cpu_start), record ? 1 : 0, capture_root ? 1 : 0,
        (unsigned long long)key_phase_end);
    fprintf(events, "key\tsent_ns\tseen_ns\n");
    for (size_t i = 0; i < 20; i++) fprintf(events, "%zu\t%llu\t%llu\n", i + 1,
        (unsigned long long)sent[i], (unsigned long long)seen[i]);
    REQUIRE(fclose(events) == 0);
    REQUIRE(snprintf(path, sizeof(path), "%s/operations.tsv", argv[2]) < (int)sizeof(path));
    FILE *actions = fopen(path, "w");
    REQUIRE(actions != NULL);
    fprintf(actions, "operation\tkind\tsent_ns\tseen_ns\tcopied_ns\tfirst_column\tmask\tattempts\ttext\n");
    for (size_t i = 0; i < operation_count; i++) {
        operation *item = &operations[i];
        fprintf(actions, "%zu\t%s\t%llu\t%llu\t%llu\t%u\t%u\t%u\t%s\n", i,
            item->kind, (unsigned long long)item->sent, (unsigned long long)item->seen,
            (unsigned long long)item->copied, item->first_column, item->mask, item->attempts, item->text);
    }
    REQUIRE(fclose(actions) == 0);
    REQUIRE(snprintf(path, sizeof(path), "%s/frames.tsv", argv[2]) < (int)sizeof(path));
    FILE *timing = fopen(path, "w");
    REQUIRE(timing != NULL);
    fprintf(timing, "frame\tstart_ns\tend_ns\tmissed\tsource\tmarker_pixel\twindow_pixel\twindow_x\twindow_y\tmarker_row\tselection_mask\n");
    REQUIRE(snprintf(path, sizeof(path), "%s/video.ffconcat", argv[2]) < (int)sizeof(path));
    FILE *concat = fopen(path, "w");
    REQUIRE(concat != NULL);
    fprintf(concat, "ffconcat version 1.0\n");
    for (size_t i = 0; i < frame_count; i++) {
        frame *current = &frames[i];
        fprintf(timing, "%zu\t%llu\t%llu\t%zu\t%zu\t%lu\t%lu\t%d\t%d\t%d\t%u\n", i,
            (unsigned long long)current->start, (unsigned long long)current->end,
            current->missed, current->source, current->marker_pixel, current->window_pixel,
            current->window_x, current->window_y, current->marker_row, current->selection_mask);
        if (current->source == i) {
            REQUIRE(snprintf(path, sizeof(path), "%s/frame-%04zu.ppm", argv[2], i) < (int)sizeof(path));
            REQUIRE(write_ppm(path, current->image));
        }
        double duration = i + 1 < frame_count
            ? (double)(frames[i + 1].end - current->end) / 1e9 : 1.0 / 60.0;
        fprintf(concat, "file frame-%04zu.ppm\noption framerate 1000\nduration %.9f\n", current->source, duration);
    }
    REQUIRE(fclose(timing) == 0 && fclose(concat) == 0);
    for (size_t i = 0; i < frame_count; i++) if (frames[i].source == i) XDestroyImage(frames[i].image);
    free(frames);
    XDestroyWindow(display, requestor);
    XCloseDisplay(display);
    printf("PASS interactive keys: record=%d frames=%zu cells=%dx%d\n", record, frame_count, cw, ch);
    return 0;
}
