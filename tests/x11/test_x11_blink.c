/* boxrig - Under MIT. */
#include "boxrig/x11.h"
#include "boxrig/ui/animation_poll.h"

#include <X11/Xutil.h>
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <string.h>

static boxrig_ui_status read_time(void *context, uint64_t *time_out)
{
    *time_out = *(uint64_t *)context;
    return BOXRIG_UI_OK;
}

static boxrig_ui_status fail_reservation(
    void *context, uint64_t deadline, uint64_t generation)
{
    (void)context;
    (void)deadline;
    (void)generation;
    return BOXRIG_UI_UNAVAILABLE;
}

static bool reject_async_reservation;
static uint64_t async_generation, async_deadline;
static boxrig_ui_status reserve_async(void *context, uint64_t deadline,
    uint64_t generation)
{
    (void)context;
    if (reject_async_reservation) return BOXRIG_UI_UNAVAILABLE;
    async_generation = generation;
    async_deadline = deadline;
    return BOXRIG_UI_OK;
}

static void cancel_reservation(void *context)
{
    (void)context;
}

static boxrig_ui_color_filter_result identity_filter(
    const boxrig_ui_color_filter_input *input, void *context)
{
    (*(unsigned *)context)++;
    return boxrig_ui_color_filter_result_default(input);
}

static bool equal_images(XImage *left, XImage *right)
{
    for (int y = 0; y < left->height; y++) {
        for (int x = 0; x < left->width; x++) {
            if (XGetPixel(left, x, y) != XGetPixel(right, x, y)) return false;
        }
    }
    return true;
}

typedef struct video_recording {
    pid_t process;
    int descriptor;
    size_t frame_bytes;
    uint8_t *frame;
} video_recording;

static int read_video_frame(video_recording *video)
{
    size_t offset = 0;
    while (offset < video->frame_bytes) {
        struct pollfd descriptor = {video->descriptor, POLLIN, 0};
        int ready;
        do {
            ready = poll(&descriptor, 1, 120000);
        } while (ready == -1 && errno == EINTR);
        if (ready <= 0) return -1;
        ssize_t count = read(video->descriptor, video->frame + offset,
            video->frame_bytes - offset);
        if (count == -1 && errno == EINTR) continue;
        if (count == 0) return offset == 0 ? 0 : -1;
        if (count < 0) return -1;
        offset += (size_t)count;
    }
    return 1;
}

static bool video_frame_matches(const video_recording *video, XImage *expected)
{
    size_t offset = 0;
    for (int y = 0; y < expected->height; y++) {
        for (int x = 0; x < expected->width; x++) {
            unsigned long pixel = XGetPixel(expected, x, y);
            if (video->frame[offset] != (uint8_t)(pixel >> 16) ||
                video->frame[offset + 1] != (uint8_t)(pixel >> 8) ||
                video->frame[offset + 2] != (uint8_t)pixel) return false;
            offset += 3;
        }
    }
    return true;
}

static void start_video(video_recording *video, const char *path,
    Display *display, unsigned width, unsigned height)
{
    int descriptors[2];
    char size[64];
    assert(pipe(descriptors) == 0);
    snprintf(size, sizeof(size), "%ux%u", width, height);
    video->frame_bytes = (size_t)width * height * 3;
    video->frame = malloc(video->frame_bytes);
    assert(video->frame != NULL);
    video->process = fork();
    assert(video->process >= 0);
    if (video->process == 0) {
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) == -1) _exit(127);
        close(descriptors[1]);
        execlp("ffmpeg", "ffmpeg", "-nostdin", "-y", "-loglevel", "error",
            "-f", "x11grab", "-draw_mouse", "0", "-framerate", "30",
            "-video_size", size, "-i", DisplayString(display),
            "-map", "0:v", "-c:v", "ffv1", "-pix_fmt", "bgr0", path,
            "-map", "0:v", "-c:v", "rawvideo", "-pix_fmt", "rgb24",
            "-f", "rawvideo", "-flush_packets", "1", "pipe:1", (char *)NULL);
        _exit(127);
    }
    close(descriptors[1]);
    video->descriptor = descriptors[0];
}

static void recorded_phase(video_recording *video, XImage *expected)
{
    if (video->process == 0) return;
    /* The next driver phase is permitted only after the recorder has emitted
     * this completed presentation. No elapsed sleep selects a test phase. */
    for (unsigned frame = 0; frame < 120; frame++) {
        assert(read_video_frame(video) == 1);
        if (video_frame_matches(video, expected)) return;
    }
    assert(!"recorded video never contained the completed phase");
}

static void stop_and_assert_video(video_recording *video, const char *path,
    XImage *on, XImage *off, unsigned cell_width, unsigned cell_height)
{
    if (video->process == 0) return;
    assert(kill(video->process, SIGINT) == 0);
    /* Drain the raw output so the recorder can flush both output streams. */
    int read_status;
    while ((read_status = read_video_frame(video)) == 1) {
    }
    assert(read_status == 0);
    close(video->descriptor);
    int status = 0;
    assert(waitpid(video->process, &status, 0) == video->process);
    assert(WIFEXITED(status) && (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 255));
    int descriptors[2];
    assert(pipe(descriptors) == 0);
    video->process = fork();
    assert(video->process >= 0);
    if (video->process == 0) {
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) == -1) _exit(127);
        close(descriptors[1]);
        execlp("ffmpeg", "ffmpeg", "-nostdin", "-loglevel", "error", "-i", path,
            "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1", (char *)NULL);
        _exit(127);
    }
    close(descriptors[1]);
    video->descriptor = descriptors[0];
    unsigned phase = 0, count = 0;
    while ((read_status = read_video_frame(video)) == 1) {
        count++;
        XImage *expected = phase == 1 ? off : on;
        if (phase < 3 && video_frame_matches(video, expected)) phase++;
        size_t offset = 0;
        for (int y = 0; y < on->height; y++) {
            for (int x = 0; x < on->width; x++) {
                if ((unsigned)x >= cell_width || (unsigned)y >= cell_height) {
                    unsigned long pixel = XGetPixel(on, x, y);
                    assert(video->frame[offset] == (uint8_t)(pixel >> 16));
                    assert(video->frame[offset + 1] == (uint8_t)(pixel >> 8));
                    assert(video->frame[offset + 2] == (uint8_t)pixel);
                }
                offset += 3;
            }
        }
    }
    assert(read_status == 0 && count >= 3 && phase == 3);
    close(video->descriptor);
    assert(waitpid(video->process, &status, 0) == video->process);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    free(video->frame);
    memset(video, 0, sizeof(*video));
}

static void advance_phase(boxrig_ui_animation_poll *adapter,
    boxrig_x11_terminal_component *component, Display *display,
    bool real_clock, uint64_t *now, uint64_t next_time, bool visible)
{
    if (!real_clock) {
        *now = next_time;
        assert(boxrig_ui_animation_poll_dispatch(adapter) == BOXRIG_UI_OK);
        return;
    }
    while (boxrig_ui_view_cursor_blink_visible(
            boxrig_x11_terminal_component_view(component)) != visible) {
        int timeout = -1;
        assert(boxrig_ui_animation_poll_prepare_wait(adapter, -1, &timeout) == BOXRIG_UI_OK);
        assert(timeout >= 0);
        struct pollfd descriptor = {ConnectionNumber(display), POLLIN, 0};
        int ready;
        do {
            ready = poll(&descriptor, 1, timeout);
        } while (ready == -1 && errno == EINTR);
        assert(ready >= 0);
        while (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);
            assert(boxrig_x11_terminal_component_dispatch_event(component, &event) !=
                   BOXRIG_UI_COMPONENT_EVENT_FAILURE);
        }
        assert(boxrig_ui_animation_poll_dispatch(adapter) == BOXRIG_UI_OK);
    }
}

int main(void)
{
    Display *display = XOpenDisplay(NULL);
    assert(display != NULL);
    Window parent = XCreateSimpleWindow(display, DefaultRootWindow(display),
        0, 0, 320, 160, 0, 0, 0);
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    boxrig_x11_terminal_component_config cc = boxrig_x11_terminal_component_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_x11_terminal_component *component = NULL;
    boxrig_ui_animation_poll *adapter = NULL;
    uint64_t now = 0;
    bool real_clock = getenv("BOXRIG_TEST_BLINK_REAL_CLOCK") != NULL;
    const char *video_path = getenv("BOXRIG_TEST_BLINK_VIDEO");
    video_recording video = {0};
    tc.rows = 3;
    tc.columns = 8;
    cc.animation_runtime = NULL;
    cc.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    cc.view.blink.cursor = real_clock ? (boxrig_ui_blink_interval){250, 250}
        : (boxrig_ui_blink_interval){10, 20};
    assert(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_x11_terminal_component_create_child(display, parent, model,
        &cc, &component) == BOXRIG_UI_UNAVAILABLE);
    assert(component == NULL);
    assert((real_clock ? boxrig_x11_animation_poll_create(&adapter)
        : boxrig_ui_animation_poll_create(read_time, &now, &adapter)) == BOXRIG_UI_OK);
    cc.animation_runtime = boxrig_ui_animation_poll_runtime(adapter);
    assert(boxrig_x11_terminal_component_create_child(display, parent, model,
        &cc, &component) == BOXRIG_UI_OK);
    boxrig_font_effective_metrics metrics;
    assert(boxrig_x11_terminal_component_get_effective_metrics(component, &metrics) ==
           BOXRIG_FONT_CATALOG_OK);
    boxrig_ui_component_allocation allocation = {0};
    allocation.width = tc.columns * metrics.cell_width;
    allocation.height = tc.rows * metrics.cell_height;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    boxrig_ui_layout_result layout;
    assert(boxrig_x11_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    assert(boxrig_x11_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    XMapWindow(display, parent);
    XSync(display, False);
    Window window = boxrig_x11_terminal_component_presentation_window(component);
    XEvent focus = {0};
    focus.type = FocusIn;
    focus.xfocus.display = display;
    focus.xfocus.window = window;
    focus.xfocus.mode = NotifyNormal;
    focus.xfocus.detail = NotifyNonlinear;
    assert(boxrig_x11_terminal_component_dispatch_event(component, &focus) ==
           BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    const uint8_t text[] = "abc\r";
    size_t consumed = 0;
    assert(boxrig_terminal_feed(terminal, text, sizeof(text) - 1, &consumed) == BOXRIG_STATUS_OK);
    unsigned color_calls = 0;
    assert(boxrig_ui_view_set_color_filter(boxrig_x11_terminal_component_view(component),
        identity_filter, &color_calls) == BOXRIG_UI_OK);
    boxrig_ui_component_frame_result frame;
    assert(boxrig_x11_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    assert(!frame.additional_dispatch);
    XSync(display, False);
    XImage *on = XGetImage(display, window, 0, 0, allocation.width, allocation.height,
        AllPlanes, ZPixmap);
    assert(on != NULL);
    unsigned warm_color_calls = color_calls;
    assert(warm_color_calls != 0);
    if (video_path != NULL) start_video(&video, video_path, display, allocation.width, allocation.height);
    recorded_phase(&video, on);
    advance_phase(adapter, component, display, real_clock, &now, 10, false);
    assert(boxrig_ui_view_frame_requested(boxrig_x11_terminal_component_view(component)));
    XImage *before_frame = XGetImage(display, window, 0, 0, allocation.width,
        allocation.height, AllPlanes, ZPixmap);
    assert(equal_images(on, before_frame));
    XDestroyImage(before_frame);
    assert(boxrig_x11_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    XSync(display, False);
    XImage *off = XGetImage(display, window, 0, 0, allocation.width, allocation.height,
        AllPlanes, ZPixmap);
    assert(off != NULL && !equal_images(on, off));
    assert(color_calls == warm_color_calls);
    for (int y = 0; y < off->height; y++) {
        for (int x = 0; x < off->width; x++) {
            if ((unsigned)y >= metrics.cell_height || (unsigned)x >= metrics.cell_width) {
                assert(XGetPixel(on, x, y) == XGetPixel(off, x, y));
            }
        }
    }
    recorded_phase(&video, off);
    advance_phase(adapter, component, display, real_clock, &now, 30, true);
    assert(boxrig_x11_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    XSync(display, False);
    XImage *again = XGetImage(display, window, 0, 0, allocation.width, allocation.height,
        AllPlanes, ZPixmap);
    assert(again != NULL && equal_images(on, again));
    assert(color_calls == warm_color_calls);
    recorded_phase(&video, again);
    stop_and_assert_video(&video, video_path, on, off, metrics.cell_width, metrics.cell_height);
    XDestroyImage(again);
    XDestroyImage(off);
    XDestroyImage(on);
    boxrig_composition *composition =
        boxrig_x11_terminal_component_composition(component);
    assert(boxrig_composition_focus_changed(composition, true) == BOXRIG_COMPOSITION_OK);
    boxrig_composition_event preedit = {0};
    preedit.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    preedit.text = "x";
    preedit.text_length = 1;
    assert(boxrig_composition_apply_event(composition, &preedit) == BOXRIG_COMPOSITION_OK);
    assert(boxrig_x11_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    uint64_t blink_deadline;
    assert(!boxrig_ui_view_next_animation_deadline(
        boxrig_x11_terminal_component_view(component), &blink_deadline));
    preedit = (boxrig_composition_event){0};
    preedit.type = BOXRIG_COMPOSITION_CANCEL;
    assert(boxrig_composition_apply_event(composition, &preedit) == BOXRIG_COMPOSITION_OK);
    assert(boxrig_x11_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_cursor_blink_visible(
        boxrig_x11_terminal_component_view(component)));
    assert(boxrig_ui_view_next_animation_deadline(
        boxrig_x11_terminal_component_view(component), &blink_deadline));
    assert(boxrig_x11_terminal_component_quiesce(component) == BOXRIG_UI_OK);
    int timeout = 0;
    assert(boxrig_ui_animation_poll_prepare_wait(adapter, -1, &timeout) == BOXRIG_UI_OK);
    assert(timeout == -1);
    if (real_clock) assert(boxrig_x11_sample_monotonic_time(&now) == 0);
    assert(boxrig_terminal_update_monotonic_time(terminal, now) == BOXRIG_STATUS_OK);
    boxrig_x11_terminal_component_destroy(component);
    boxrig_ui_animation_poll_destroy(adapter);
    boxrig_ui_animation_scheduler failing_scheduler = {
        &now, read_time, fail_reservation, cancel_reservation
    };
    boxrig_ui_animation_runtime *failing_runtime = NULL;
    assert(boxrig_ui_animation_runtime_create(&failing_scheduler, &failing_runtime) ==
        BOXRIG_UI_OK);
    cc.animation_runtime = failing_runtime;
    assert(boxrig_x11_terminal_component_create_child(display, parent, model,
        &cc, &component) == BOXRIG_UI_OK);
    assert(boxrig_x11_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    focus.xfocus.window = boxrig_x11_terminal_component_presentation_window(component);
    assert(boxrig_x11_terminal_component_dispatch_event(component, &focus) ==
        BOXRIG_UI_COMPONENT_EVENT_FAILURE);
    assert(!boxrig_ui_view_focused(boxrig_x11_terminal_component_view(component)));
    assert(!boxrig_x11_terminal_component_focused(component));
    boxrig_x11_terminal_component_destroy(component);
    boxrig_ui_animation_runtime_destroy(failing_runtime);
    failing_scheduler.reserve = reserve_async;
    assert(boxrig_ui_animation_runtime_create(&failing_scheduler, &failing_runtime) == BOXRIG_UI_OK);
    cc.animation_runtime = failing_runtime;
    assert(boxrig_x11_terminal_component_create_child(display, parent, model,
        &cc, &component) == BOXRIG_UI_OK);
    assert(boxrig_x11_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    focus.xfocus.window = boxrig_x11_terminal_component_presentation_window(component);
    assert(boxrig_x11_terminal_component_dispatch_event(component, &focus) != BOXRIG_UI_COMPONENT_EVENT_FAILURE);
    now = async_deadline;
    reject_async_reservation = true;
    assert(boxrig_ui_animation_runtime_dispatch(failing_runtime, async_generation) == BOXRIG_UI_UNAVAILABLE);
    boxrig_ui_component_diagnostic diagnostic;
    assert(boxrig_x11_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
    assert(diagnostic.failed && !diagnostic.running);
    assert(diagnostic.failure_reason == BOXRIG_UI_COMPONENT_FAILURE_ANIMATION);
    boxrig_x11_terminal_component_destroy(component);
    boxrig_ui_animation_runtime_destroy(failing_runtime);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, parent);
    XCloseDisplay(display);
    cairo_debug_reset_static_data();
    FcFini();
    puts("PASS X11 cursor phases, frame requests and scheduler ownership");
    return 0;
}
