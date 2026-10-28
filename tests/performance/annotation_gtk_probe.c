/* boxrig - Under MIT. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "probe:%d: %s\n", __LINE__, #value); return 1; } } while (0)

static uint64_t now_ns(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) abort();
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static void pause_ms(long milliseconds)
{
    struct timespec duration = {milliseconds / 1000, milliseconds % 1000 * 1000000L};
    while (nanosleep(&duration, &duration) != 0 && errno == EINTR) { }
}

static bool send_key(Display *display, KeySym symbol)
{
    KeyCode code = XKeysymToKeycode(display, symbol);
    if (code == 0 || !XTestFakeKeyEvent(display, code, True, CurrentTime) ||
        !XTestFakeKeyEvent(display, code, False, CurrentTime)) return false;
    XFlush(display);
    return true;
}

static XImage *capture(Display *display, Window window, int width, int height)
{
    XImage *image = XGetImage(display, window, 0, 0,
        (unsigned int)width, (unsigned int)height, AllPlanes, ZPixmap);
    if (image == NULL || image->red_mask != 0xff0000 ||
        image->green_mask != 0xff00 || image->blue_mask != 0xff) {
        if (image != NULL) XDestroyImage(image);
        return NULL;
    }
    return image;
}

static bool save_ppm(const char *path, const XImage *image)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    bool good = fprintf(file, "P6\n%d %d\n255\n", image->width, image->height) > 0;
    unsigned char *row = malloc((size_t)image->width * 3);
    if (row == NULL) good = false;
    for (int y = 0; y < image->height && good; y++) {
        for (int x = 0; x < image->width; x++) {
            unsigned long pixel = XGetPixel((XImage *)image, x, y);
            row[x * 3] = (unsigned char)(pixel >> 16);
            row[x * 3 + 1] = (unsigned char)(pixel >> 8);
            row[x * 3 + 2] = (unsigned char)pixel;
        }
        good = fwrite(row, 3, (size_t)image->width, file) == (size_t)image->width;
    }
    free(row);
    return fclose(file) == 0 && good;
}

static bool grid(const XImage *image, int *x_out, int *y_out, int *cw_out, int *ch_out)
{
    for (int y = 0; y < image->height; y++) {
        for (int x = 0; x < image->width; x++) {
            if ((XGetPixel((XImage *)image, x, y) & 0xffffff) != 0x405050) continue;
            int width = 0, height = 0;
            while (x + width < image->width &&
                (XGetPixel((XImage *)image, x + width, y) & 0xffffff) == 0x405050) width++;
            while (y + height < image->height &&
                (XGetPixel((XImage *)image, x, y + height) & 0xffffff) == 0x405050) height++;
            if (width < 40 || width % 8 != 0 || height < 8) continue;
            int second = y + height;
            while (second < image->height &&
                (XGetPixel((XImage *)image, x, second) & 0xffffff) != 0x405050) second++;
            if (second == image->height ||
                ((second - y) != 7 * height && (second - y) != 16 * height)) continue;
            *x_out = x; *y_out = (second - y) == 7 * height ? y : y - 7 * height;
            *cw_out = width / 8; *ch_out = height;
            return true;
        }
    }
    return false;
}

static int underline_run(const XImage *image, int gx, int gy, int cw, int ch, int row)
{
    int best = 0;
    int left = gx + 11 * cw, right = gx + 47 * cw;
    if (right >= image->width || gy + row * ch >= image->height) return -1;
    for (int y = gy + (row - 1) * ch + ch / 2; y < gy + row * ch; y++) {
        int run = 0;
        for (int x = left; x < right; x++) {
            unsigned long pixel = XGetPixel((XImage *)image, x, y) & 0xffffff;
            if (pixel == 0xffffff) run++;
            else run = 0;
            if (run > best) best = run;
        }
    }
    return best;
}

static unsigned int changed_pixels(const XImage *left, const XImage *right,
    int gx, int gy, int cw, int ch, int row)
{
    unsigned int changed = 0;
    for (int y = gy + (row - 1) * ch; y < gy + row * ch; y++) {
        for (int x = gx + 11 * cw; x < gx + 48 * cw; x++) {
            if (XGetPixel((XImage *)left, x, y) != XGetPixel((XImage *)right, x, y)) changed++;
        }
    }
    return changed;
}

static unsigned int bright_pixels(const XImage *image,
    int gx, int gy, int cw, int ch, int row)
{
    unsigned int bright = 0;
    for (int y = gy + (row - 1) * ch; y < gy + row * ch; y++) {
        for (int x = gx + 11 * cw; x < gx + 48 * cw; x++) {
            if ((XGetPixel((XImage *)image, x, y) & 0xffffff) != 0) bright++;
        }
    }
    return bright;
}

static bool record_frame(const char *directory, unsigned int *index, FILE *frames,
    const char *phase, uint64_t timestamp, const XImage *image);

static XImage *wait_visual(Display *display, Window window, int width, int height,
    int gx, int gy, int cw, int ch, int row, bool present, const XImage *different,
    bool video, const char *directory, unsigned int *frame_index, FILE *frames,
    const char *phase)
{
    uint64_t deadline = now_ns() + UINT64_C(30000000000);
    while (now_ns() < deadline) {
        uint64_t sampled = now_ns();
        XImage *image = capture(display, window, width, height);
        if (image == NULL) return NULL;
        if (video && !record_frame(directory, frame_index, frames, phase, sampled, image)) {
            XDestroyImage(image);
            return NULL;
        }
        int run = underline_run(image, gx, gy, cw, ch, row);
        bool matches = present ? run >= cw * 2 :
            run < cw * 2 && bright_pixels(image, gx, gy, cw, ch, row) == 0;
        if (matches && (different == NULL ||
            changed_pixels(different, image, gx, gy, cw, ch, row) > 0)) return image;
        XDestroyImage(image);
        pause_ms(16);
    }
    XImage *diagnostic = capture(display, window, width, height);
    if (diagnostic != NULL) {
        char path[512];
        snprintf(path, sizeof(path), "%s/timeout-%s.ppm", directory, phase);
        (void)save_ppm(path, diagnostic);
        fprintf(stderr, "visual timeout phase=%s row=%d run=%d bright=%u\n",
            phase, row, underline_run(diagnostic, gx, gy, cw, ch, row),
            bright_pixels(diagnostic, gx, gy, cw, ch, row));
        XDestroyImage(diagnostic);
    }
    return NULL;
}

static bool record_frame(const char *directory, unsigned int *index, FILE *frames,
    const char *phase, uint64_t timestamp, const XImage *image)
{
    char path[512];
    int length = snprintf(path, sizeof(path), "%s/frame-%05u.ppm", directory, *index);
    if (length <= 0 || (size_t)length >= sizeof(path) || !save_ppm(path, image)) return false;
    fprintf(frames, "%u\t%s\t%" PRIu64 "\n", *index, phase, timestamp);
    (*index)++;
    return fflush(frames) == 0;
}

static bool capture_interval(Display *display, Window window, int width, int height,
    const char *directory, unsigned int *frame_index, FILE *frames,
    const char *phase, long milliseconds, bool video)
{
    if (!video) { pause_ms(milliseconds); return true; }
    uint64_t deadline = now_ns() + (uint64_t)milliseconds * UINT64_C(1000000);
    while (now_ns() < deadline) {
        uint64_t sampled = now_ns();
        XImage *image = capture(display, window, width, height);
        if (image == NULL) return false;
        bool good = record_frame(directory, frame_index, frames, phase, sampled, image);
        XDestroyImage(image);
        if (!good) return false;
        uint64_t next = sampled + UINT64_C(16666667);
        uint64_t current = now_ns();
        if (next > current) pause_ms((long)((next - current + UINT64_C(999999)) / UINT64_C(1000000)));
    }
    return true;
}

static bool wait_marker(Display *display, Window window, int width, int height,
    int gx, int gy, int cw, int ch, unsigned int sequence, uint64_t *seen,
    bool video, const char *directory, unsigned int *frame_index, FILE *frames)
{
    uint64_t deadline = now_ns() + UINT64_C(5000000000);
    while (now_ns() < deadline) {
        int footer_x = gx + 3 * cw, footer_y = gy + 23 * ch + ch / 2;
        XImage *image = video ? capture(display, window, width, height) :
            XGetImage(display, window, footer_x, footer_y, 1, 1, AllPlanes, ZPixmap);
        if (image == NULL) return false;
        uint64_t timestamp = now_ns();
        if (video && !record_frame(directory, frame_index, frames, "key", timestamp, image)) {
            XDestroyImage(image);
            return false;
        }
        unsigned long pixel = XGetPixel(image, video ? footer_x : 0,
            video ? footer_y : 0) & 0xffffff;
        XDestroyImage(image);
        if (pixel == ((64u + sequence * 5u) << 16 | 0x5050u)) {
            *seen = timestamp;
            return true;
        }
        pause_ms(10);
    }
    return false;
}

static bool wait_stop_barrier(Display *display, Window window, int width, int height,
    int gx, int gy, int cw, int ch, bool video, const char *directory,
    unsigned int *frame_index, FILE *frames, uint64_t *seen_out)
{
    uint64_t deadline = now_ns() + UINT64_C(15000000000);
    int x = gx + 62 * cw, y = gy + 23 * ch + ch / 2;
    while (now_ns() < deadline) {
        XImage *image = video ? capture(display, window, width, height) :
            XGetImage(display, window, x, y, 1, 1, AllPlanes, ZPixmap);
        if (image == NULL) return false;
        if (video && !record_frame(directory, frame_index, frames,
            "stop-barrier", now_ns(), image)) { XDestroyImage(image); return false; }
        unsigned long pixel = XGetPixel(image, video ? x : 0, video ? y : 0) & 0xffffff;
        XDestroyImage(image);
        if (pixel == 0x20a0e0) { *seen_out = now_ns(); return true; }
        pause_ms(16);
    }
    return false;
}

int main(int argc, char **argv)
{
    CHECK(argc == 4);
    Window window = (Window)strtoul(argv[1], NULL, 0);
    bool video = strcmp(argv[3], "video") == 0;
    CHECK(video || strcmp(argv[3], "none") == 0);
    Display *display = XOpenDisplay(NULL);
    CHECK(display != NULL);
    int event, error, major, minor;
    CHECK(XTestQueryExtension(display, &event, &error, &major, &minor));
    XWindowAttributes attributes;
    CHECK(XGetWindowAttributes(display, window, &attributes));
    XSetInputFocus(display, window, RevertToParent, CurrentTime);
    XSync(display, False);
    int gx = 0, gy = 0, cw = 0, ch = 0;
    uint64_t deadline = now_ns() + UINT64_C(10000000000);
    while (now_ns() < deadline && cw == 0) {
        XImage *image = capture(display, window, attributes.width, attributes.height);
        CHECK(image != NULL);
        (void)grid(image, &gx, &gy, &cw, &ch);
        XDestroyImage(image);
        if (cw == 0) pause_ms(20);
    }
    if (!(cw > 0 && ch > 0 && gy + 24 * ch <= attributes.height)) {
        fprintf(stderr, "grid x=%d y=%d cell=%dx%d window=%dx%d\n",
            gx, gy, cw, ch, attributes.width, attributes.height);
        XImage *diagnostic = capture(display, window, attributes.width, attributes.height);
        if (diagnostic != NULL) {
            char diagnostic_path[512];
            snprintf(diagnostic_path, sizeof(diagnostic_path), "%s/grid-failure.ppm", argv[2]);
            (void)save_ppm(diagnostic_path, diagnostic);
            XDestroyImage(diagnostic);
        }
        CHECK(false);
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/probe.tsv", argv[2]);
    FILE *report = fopen(path, "w");
    CHECK(report != NULL);
    snprintf(path, sizeof(path), "%s/frames.tsv", argv[2]);
    FILE *frames = video ? fopen(path, "w") : NULL;
    CHECK(!video || frames != NULL);
    if (frames != NULL) fprintf(frames, "frame\tphase\ttimestamp_ns\n");
    unsigned int frame_index = 0;
    fprintf(report, "key\tsent_ns\tseen_ns\tphase_id\n");
    /* A painted, recognized URL establishes that the history feed has been
     * consumed before latency sampling begins. */
    CHECK(send_key(display, XK_c));
    XImage *ready = wait_visual(display, window, attributes.width, attributes.height,
        gx, gy, cw, ch, 3, true, NULL, video, argv[2], &frame_index, frames, "ready");
    CHECK(ready != NULL);
    XDestroyImage(ready);
    CHECK(send_key(display, XK_z));
    XImage *ready_blank = wait_visual(display, window, attributes.width, attributes.height,
        gx, gy, cw, ch, 3, false, NULL, video, argv[2], &frame_index, frames, "ready-clear");
    CHECK(ready_blank != NULL);
    XDestroyImage(ready_blank);
    const char *load = getenv("BOXRIG_ANNOTATION_GTK_LOAD");
    const char *keys_text = getenv("BOXRIG_ANNOTATION_GTK_KEYS");
    char *keys_end = NULL;
    unsigned long key_count = keys_text == NULL ? 3 : strtoul(keys_text, &keys_end, 10);
    CHECK(key_count >= 1 && key_count <= 30 &&
        (keys_text == NULL || (*keys_text != '\0' && *keys_end == '\0')));
    bool transcript = load != NULL && strcmp(load, "transcript") == 0;
    if (transcript) {
        CHECK(send_key(display, XK_g));
        CHECK(capture_interval(display, window, attributes.width, attributes.height,
            argv[2], &frame_index, frames, "load-start", 100, video));
    }
    for (unsigned int i = 1; i <= key_count; i++) {
        unsigned int phase = 0;
        if (transcript && i > key_count / 2) {
            if (i == key_count / 2 + 1) {
                CHECK(send_key(display, XK_x));
                phase = 1;
            } else {
                phase = 2;
            }
        }
        uint64_t sent = now_ns(), seen = 0;
        CHECK(send_key(display, XK_k));
        CHECK(wait_marker(display, window, attributes.width, attributes.height,
            gx, gy, cw, ch, i, &seen, video, argv[2], &frame_index, frames));
        fprintf(report, "%u\t%" PRIu64 "\t%" PRIu64 "\t%u\n", i, sent, seen, phase);
        if (phase == 1) {
            uint64_t barrier_seen = 0;
            CHECK(wait_stop_barrier(display, window,
                attributes.width, attributes.height, gx, gy, cw, ch,
                video, argv[2], &frame_index, frames, &barrier_seen));
            fprintf(report, "# stop_barrier_seen_ns=%" PRIu64 "\n", barrier_seen);
        }
        if (phase == 1) CHECK(capture_interval(display, window,
            attributes.width, attributes.height, argv[2], &frame_index,
            frames, "drain", 1000, video));
        CHECK(capture_interval(display, window, attributes.width, attributes.height,
            argv[2], &frame_index, frames, "between-keys", 50, video));
    }
    if (!transcript) CHECK(send_key(display, XK_x));
    CHECK(capture_interval(display, window, attributes.width, attributes.height,
        argv[2], &frame_index, frames, "load-stop", 100, video));
    CHECK(send_key(display, XK_z));
    XImage *before = wait_visual(display, window, attributes.width, attributes.height,
        gx, gy, cw, ch, 3, false, NULL, video, argv[2], &frame_index, frames, "clear");
    CHECK(before != NULL);
    if (video) CHECK(record_frame(argv[2], &frame_index, frames, "before", now_ns(), before));
    CHECK(send_key(display, XK_c));
    XImage *created = wait_visual(display, window, attributes.width, attributes.height,
        gx, gy, cw, ch, 3, true, before, video, argv[2], &frame_index, frames, "create");
    CHECK(created != NULL);
    if (video) CHECK(record_frame(argv[2], &frame_index, frames, "created", now_ns(), created));
    int initial_run = underline_run(created, gx, gy, cw, ch, 3);
    int blank_run = underline_run(before, gx, gy, cw, ch, 3);
    fprintf(report, "# underline_before=%d created=%d cell_width=%d\n", blank_run, initial_run, cw);
    CHECK(initial_run >= cw * 2 && blank_run < cw * 2);
    CHECK(send_key(display, XK_m));
    XImage *modified = wait_visual(display, window, attributes.width, attributes.height,
        gx, gy, cw, ch, 3, true, created, video, argv[2], &frame_index, frames, "modify");
    CHECK(modified != NULL);
    if (video) CHECK(record_frame(argv[2], &frame_index, frames, "modified", now_ns(), modified));
    int modified_run = underline_run(modified, gx, gy, cw, ch, 3);
    unsigned int changed = changed_pixels(created, modified, gx, gy, cw, ch, 3);
    fprintf(report, "# underline_modified=%d modified_pixels=%u\n", modified_run, changed);
    CHECK(modified_run >= cw * 2 && changed > 0);
    CHECK(send_key(display, XK_s));
    XImage *scrolled = wait_visual(display, window, attributes.width, attributes.height,
        gx, gy, cw, ch, 2, true, NULL, video, argv[2], &frame_index, frames, "scroll");
    CHECK(scrolled != NULL);
    if (video) CHECK(record_frame(argv[2], &frame_index, frames, "scrolled", now_ns(), scrolled));
    int moved_run = underline_run(scrolled, gx, gy, cw, ch, 2);
    fprintf(report, "# underline_scrolled=%d\n", moved_run);
    CHECK(moved_run >= cw * 2);
    CHECK(send_key(display, XK_e));
    XImage *erased = wait_visual(display, window, attributes.width, attributes.height,
        gx, gy, cw, ch, 2, false, scrolled, video, argv[2], &frame_index, frames, "erase");
    CHECK(erased != NULL);
    if (video) CHECK(record_frame(argv[2], &frame_index, frames, "erased", now_ns(), erased));
    int erased_run = underline_run(erased, gx, gy, cw, ch, 2);
    fprintf(report, "# underline_erased=%d\n", erased_run);
    CHECK(erased_run < cw * 2);
    XDestroyImage(before); XDestroyImage(created); XDestroyImage(modified);
    XDestroyImage(scrolled); XDestroyImage(erased);
    CHECK(send_key(display, XK_q));
    CHECK(fclose(report) == 0);
    CHECK(!video || fclose(frames) == 0);
    XCloseDisplay(display);
    return 0;
}
