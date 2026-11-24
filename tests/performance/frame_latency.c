/* boxrig - Under MIT. Observe actual X11 pixels, independently of render calls. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "latency:%d: %s\n", __LINE__, #c); return 1; } } while (0)

static uint64_t now_ns(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) abort();
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
}

static void pause_ms(long value)
{
    struct timespec delay = {value / 1000, value % 1000 * 1000000};
    while (nanosleep(&delay, &delay) != 0) {}
}

static FILE *open_output(const char *directory, const char *name, const char *mode)
{
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
    return length > 0 && (size_t)length < sizeof(path) ? fopen(path, mode) : NULL;
}

static unsigned long image_color(unsigned int sequence)
{
    const unsigned long colors[] = {0xff0000, 0x00ff00, 0x0000ff, 0x000000};
    return colors[sequence % 4];
}

static int wait_for_observation(FILE *control, FILE *burst, unsigned int sequence)
{
    if (burst != NULL) {
        struct pollfd descriptors[] = {
            {.fd = fileno(control), .events = POLLIN},
            {.fd = STDOUT_FILENO, .events = POLLOUT}};
        size_t batches = 0;
        /* Keep changing a separate cell until the observer has seen the
         * numbered update. A renderer waiting for quiet cannot make progress. */
        for (;;) {
            int ready = poll(descriptors, 2, -1);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0 || (descriptors[1].revents & (POLLERR | POLLHUP))) return EOF;
            if (descriptors[0].revents != 0) break;
            for (unsigned int i = 0; i < 32; i++)
                if (fputs(i % 2 ? "\033[1;40HX" : "\033[1;40HO", stdout) == EOF) return EOF;
            if (fflush(stdout) != 0) return EOF;
            batches++;
        }
        fprintf(burst, "%u\t%zu\n", sequence, batches);
    }
    return fgetc(control);
}

static int peer(const char *directory)
{
    struct winsize size;
    CHECK(ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col >= 40 && size.ws_row >= 12);
    FILE *report = open_output(directory, "peer.tsv", "w");
    CHECK(report != NULL);
    fputs("sequence\temitted_ns\twritten_ns\n", report);
    FILE *control = NULL;
    if (getenv("BOXRIG_LATENCY_ISOLATED_CELL") != NULL) {
        bool key_echo = getenv("BOXRIG_LATENCY_KEY_ECHO") != NULL;
        /* With autowrap disabled the cursor stays on the last column. Every
         * measured event changes exactly one printable byte at that cell. */
        printf("\033[?7l\033[?25l\033[H\033[48;2;64;80;80m        "
            "\033[2;%uH\033[38;2;255;255;255;48;2;0;0;0m", size.ws_col);
        CHECK(fflush(stdout) == 0);
        for (unsigned int sequence = 0; sequence < 24; sequence++) {
            if (sequence != 0) CHECK(fgetc(key_echo ? stdin : control) == 'n');
            uint64_t start = now_ns();
            CHECK(fputc(sequence % 2 == 0 ? 'M' : ' ', stdout) != EOF && fflush(stdout) == 0);
            fprintf(report, "%u\t%" PRIu64 "\t%" PRIu64 "\n", sequence, start, now_ns());
            CHECK(fflush(report) == 0);
            if (sequence == 0) {
                control = open_output(directory, "control.fifo", "r");
                CHECK(control != NULL);
            }
        }
        CHECK(fgetc(control) == 'q');
        CHECK(fclose(control) == 0 && fclose(report) == 0);
        return 0;
    }
    FILE *burst = NULL;
    if (getenv("BOXRIG_LATENCY_CONTINUOUS") != NULL) {
        burst = open_output(directory, "burst.tsv", "w");
        CHECK(burst != NULL);
    }
    const char *rgba[] = {"/wAA/w==", "AP8A/w==", "AAD//w=="};
    fputs("\033]11;#000000\033\\", stdout);
    for (unsigned int sequence = 0; sequence < 24; sequence++) {
        if (sequence != 0) CHECK(wait_for_observation(control, burst, sequence - 1) == 'n');
        uint64_t start = now_ns();
        fputs("\033[?2026h\033[?7l\033[?25l\033[0m\033[2J\033[H", stdout);
        /* ED already removed the placement. Delete the resource by ID, since
         * Kitty's 'A' selector only selects images that still have placements. */
        if (sequence % 4 == 3) fputs("\033_Ga=d,d=I,i=17,q=2\033\\", stdout);
        else printf("\033[2;1H\033_Ga=T,f=32,s=1,v=1,i=17,p=1,c=%u,r=%u,z=-1,C=1,q=2;%s\033\\",
            size.ws_col, size.ws_row - 1, rgba[sequence % 4]);
        for (unsigned int row = 2; row <= size.ws_row; row++) {
            printf("\033[%u;1H", row);
            for (unsigned int column = 0; column < size.ws_col; column++) {
                if (row == 2 && column == 30) fputc(' ', stdout);
                else fputs((column + sequence) % 2 == 0 ? "\033[38;2;240;64;192mM" :
                    "\033[38;2;64;224;192mM", stdout);
            }
        }
        printf("\033[1;1H\033[48;2;%u;80;80m        \033[0m frame %02u\033[?2026l",
            64 + sequence * 5, sequence);
        CHECK(fflush(stdout) == 0);
        fprintf(report, "%u\t%" PRIu64 "\t%" PRIu64 "\n", sequence, start, now_ns());
        CHECK(fflush(report) == 0);
        if (sequence == 0) {
            control = open_output(directory, "control.fifo", "r");
            CHECK(control != NULL);
        }
    }
    CHECK(wait_for_observation(control, burst, 23) == 'q');
    if (burst != NULL) CHECK(fclose(burst) == 0);
    CHECK(fclose(control) == 0 && fclose(report) == 0);
    return 0;
}

static bool save_image(const char *directory, const char *name, XImage *image)
{
    FILE *output = open_output(directory, name, "wb");
    if (output == NULL) return false;
    bool good = fprintf(output, "P6\n%d %d\n255\n", image->width, image->height) > 0;
    uint8_t *row = malloc((size_t)image->width * 3);
    if (row == NULL) good = false;
    for (int y = 0; good && y < image->height; y++) {
        for (int x = 0; x < image->width; x++) {
            unsigned long pixel = XGetPixel(image, x, y);
            row[x*3] = (uint8_t)(pixel >> 16);
            row[x*3+1] = (uint8_t)(pixel >> 8);
            row[x*3+2] = (uint8_t)pixel;
        }
        good = fwrite(row, 3, (size_t)image->width, output) == (size_t)image->width;
    }
    free(row);
    return fclose(output) == 0 && good;
}

static bool save_frame(const char *directory, size_t index, XImage *image)
{
    char name[64];
    snprintf(name, sizeof(name), "frame-%05zu.ppm", index);
    return save_image(directory, name, image);
}

static unsigned long rss_kib(const char *pid)
{
    char path[256], line[256];
    snprintf(path, sizeof(path), "/proc/%s/status", pid);
    FILE *input = fopen(path, "r");
    if (input == NULL) return 0;
    unsigned long rss = 0;
    while (fgets(line, sizeof(line), input) != NULL)
        if (sscanf(line, "VmRSS: %lu", &rss) == 1) break;
    fclose(input);
    return rss;
}

static bool record_host_cpu(FILE *output, unsigned int sequence)
{
    FILE *input = fopen("/proc/stat", "r");
    if (input == NULL) return false;
    uint64_t ticks[8], total = 0;
    int fields = fscanf(input, "cpu %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
        " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64,
        &ticks[0], &ticks[1], &ticks[2], &ticks[3], &ticks[4], &ticks[5], &ticks[6], &ticks[7]);
    if (fclose(input) != 0 || fields != 8) return false;
    /* Guest ticks are already included in user and nice. Count idle and
     * iowait together, as in the live terminal process measurements. */
    for (size_t i = 0; i < 8; i++) total += ticks[i];
    return fprintf(output, "%u\t%" PRIu64 "\t%" PRIu64 "\n", sequence, total, ticks[3] + ticks[4]) > 0;
}

static int probe(char **argv)
{
    Window window = (Window)strtoul(argv[2], NULL, 0);
    const char *directory = argv[3];
    int rows = atoi(argv[4]), columns = atoi(argv[5]);
    int cw = atoi(argv[6]), ch = atoi(argv[7]);
    CHECK(rows >= 12 && columns >= 40 && cw > 0 && ch > 0);
    Display *display = XOpenDisplay(NULL);
    CHECK(display != NULL);
    XWindowAttributes attributes;
    CHECK(XGetWindowAttributes(display, window, &attributes));
    FILE *report = open_output(directory, "probe.tsv", "w");
    FILE *frames = open_output(directory, "frames.tsv", "w");
    FILE *host_cpu = open_output(directory, "host-cpu.tsv", "w");
    CHECK(report != NULL && frames != NULL && host_cpu != NULL);
    fputs("sequence\ttotal_ticks\tidle_ticks\n", host_cpu);
    FILE *unstable = NULL;
    size_t disagreements = 0;
    if (getenv("BOXRIG_VERIFY_STABLE_READBACK") != NULL) {
        unstable = open_output(directory, "unstable.tsv", "w");
        CHECK(unstable != NULL);
        fputs("index\tfirst_ns\tsecond_ns\n", unstable);
    }
    const char *period = getenv("BOXRIG_REPLAY_PERIOD_MS");
    uint64_t period_ns = 0;
    clockid_t terminal_clock = 0;
    FILE *cpu = NULL;
    if (period != NULL) {
        char *end;
        unsigned long ms = strtoul(period, &end, 10);
        CHECK(*period != '\0' && *end == '\0' && ms > 0 && ms <= 60000);
        period_ns = (uint64_t)ms * 1000000;
        CHECK(clock_getcpuclockid((pid_t)strtol(argv[8], NULL, 10), &terminal_clock) == 0);
        cpu = open_output(directory, "terminal-cpu.tsv", "w");
        CHECK(cpu != NULL);
        fputs("sequence\tprocess_cpu_ns\n", cpu);
    }
    fputs("sequence\tsent_ns\tseen_ns\trss_kib\n", report);
    fputs("frame\ttimestamp_ns\tsequence\timage_pixel\tink\n", frames);
    int gx = -1, gy = -1;
    bool isolated = getenv("BOXRIG_LATENCY_ISOLATED_CELL") != NULL;
    bool key_echo = isolated && getenv("BOXRIG_LATENCY_KEY_ECHO") != NULL;
    if (key_echo) {
        XSetInputFocus(display, window, RevertToParent, CurrentTime);
        XSync(display, False);
    }
    size_t frame_index = 0;
    uint64_t diagnostic_time = 0;
    FILE *control = NULL;
    for (unsigned int sequence = 0; sequence < 24; sequence++) {
        uint64_t sent = now_ns();
        if (sequence != 0 && key_echo) {
            /* GTK receives device key events through XInput. Send the same
             * focused keyboard event for both GTK and native X11 samples. */
            Window focus;
            int revert;
            XGetInputFocus(display, &focus, &revert);
            CHECK(focus == window);
            KeyCode key = XKeysymToKeycode(display, XK_n);
            CHECK(key != 0 && XTestFakeKeyEvent(display, key, True, CurrentTime) &&
                XTestFakeKeyEvent(display, key, False, CurrentTime));
            XFlush(display);
        } else if (sequence != 0) {
            CHECK(fputc('n', control) != EOF && fflush(control) == 0);
        }
        uint64_t deadline = now_ns() + UINT64_C(30000000000);
        bool seen = false;
        while (!seen && now_ns() < deadline) {
            XImage *image = XGetImage(display, window, 0, 0,
                (unsigned int)attributes.width, (unsigned int)attributes.height, AllPlanes, ZPixmap);
            uint64_t sampled = now_ns();
            CHECK(image != NULL && image->red_mask == 0xff0000 &&
                image->green_mask == 0xff00 && image->blue_mask == 0xff);
            if (unstable != NULL && gx >= 0) {
                /* XGetImage samples a live drawable. During a presentation
                 * it can return rows from two successive buffers. Keep both
                 * raw reads in that case and time a confirmed picture at
                 * the end of two identical reads. */
                XImage *next = XGetImage(display, window, 0, 0,
                    (unsigned int)attributes.width, (unsigned int)attributes.height, AllPlanes, ZPixmap);
                uint64_t confirmed = now_ns();
                CHECK(next != NULL && image->bytes_per_line == next->bytes_per_line);
                if (memcmp(image->data, next->data, (size_t)image->bytes_per_line * image->height) != 0) {
                    char name[64];
                    snprintf(name, sizeof(name), "unstable-%05zu-first.ppm", disagreements);
                    CHECK(save_image(directory, name, image));
                    snprintf(name, sizeof(name), "unstable-%05zu-second.ppm", disagreements);
                    CHECK(save_image(directory, name, next));
                    fprintf(unstable, "%zu\t%" PRIu64 "\t%" PRIu64 "\n", disagreements++, sampled, confirmed);
                    XDestroyImage(next);
                    XDestroyImage(image);
                    continue;
                }
                XDestroyImage(image);
                image = next;
                sampled = confirmed;
            }
            if (gx < 0) {
                for (int y = 0; y + rows * ch <= image->height && gx < 0; y++)
                    for (int x = 0; x + columns * cw <= image->width; x++)
                        if ((XGetPixel(image, x, y) & 0xffffff) == 0x405050 &&
                            (XGetPixel(image, x + 8*cw - 1, y + ch - 1) & 0xffffff) == 0x405050) {
                            gx = x; gy = y; break;
                        }
                if (gx >= 0) fprintf(report, "# grid=%d,%d,%d,%d rows=%d columns=%d\n", gx, gy, cw, ch, rows, columns);
                else if (sampled - diagnostic_time >= UINT64_C(1000000000)) {
                    CHECK(save_frame(directory, 0, image));
                    diagnostic_time = sampled;
                }
            }
            if (gx >= 0) {
                unsigned long marker = XGetPixel(image, gx + 3*cw, gy + ch/2) & 0xffffff;
                int observed = (marker & 0xffff) == 0x5050 && (marker >> 16) >= 64 &&
                    ((marker >> 16) - 64) % 5 == 0 ? (int)(((marker >> 16) - 64) / 5) : -1;
                unsigned long background = isolated ? 0 :
                    XGetPixel(image, gx + 30*cw + cw/2, gy + ch + ch/2) & 0xffffff;
                int ink_x = isolated ? gx + (columns - 1)*cw : gx;
                size_t ink = 0;
                for (int y = gy + ch; y < gy + 2*ch; y++)
                    for (int x = ink_x; x < ink_x + cw; x++)
                        if ((XGetPixel(image, x, y) & 0xffffff) != background) ink++;
                if (isolated) observed = ((sequence % 2 == 0) == (ink > 0)) ? (int)sequence : (int)sequence - 1;
                CHECK(save_frame(directory, frame_index, image));
                fprintf(frames, "%zu\t%" PRIu64 "\t%d\t%lu\t%zu\n",
                    frame_index++, sampled, observed, background, ink);
                CHECK(fflush(frames) == 0);
                if (observed == (int)sequence && (isolated || (background == image_color(sequence) && ink > 0))) {
                    if (sequence == 0 || sequence == 23) CHECK(record_host_cpu(host_cpu, sequence));
                    unsigned long rss = rss_kib(argv[8]);
                    CHECK(rss != 0);
                    fprintf(report, "%u\t%" PRIu64 "\t%" PRIu64 "\t%lu\n", sequence, sent, sampled, rss);
                    CHECK(fflush(report) == 0);
                    if (cpu != NULL) {
                        struct timespec value;
                        CHECK(clock_gettime(terminal_clock, &value) == 0);
                        fprintf(cpu, "%u\t%" PRIu64 "\n", sequence,
                            (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec);
                    }
                    seen = true;
                }
            }
            XDestroyImage(image);
            if (!seen) pause_ms(5);
        }
        CHECK(seen);
        if (sequence == 0) {
            control = open_output(directory, "control.fifo", "w");
            CHECK(control != NULL);
        }
        if (period_ns == 0) pause_ms(17);
        else if (sequence + 1 < 24) {
            uint64_t next = sent + period_ns;
            struct timespec deadline = {(time_t)(next / 1000000000), (long)(next % 1000000000)};
            int result;
            do { result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL); } while (result == EINTR);
            CHECK(result == 0);
        }
    }
    CHECK(fputc('q', control) != EOF && fflush(control) == 0);
    CHECK(fclose(control) == 0 && fclose(report) == 0 && fclose(frames) == 0);
    if (cpu != NULL) CHECK(fclose(cpu) == 0);
    if (unstable != NULL) CHECK(fclose(unstable) == 0);
    CHECK(fclose(host_cpu) == 0);
    XCloseDisplay(display);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "peer") == 0) return peer(argv[2]);
    if (argc == 9 && strcmp(argv[1], "probe") == 0) return probe(argv);
    fprintf(stderr, "usage: %s peer DIRECTORY | probe WINDOW DIRECTORY ROWS COLUMNS CELL-WIDTH CELL-HEIGHT TERMINAL-PID\n", argv[0]);
    return 2;
}
