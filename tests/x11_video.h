/* Optional recording for native UI assertions. Set BOXRIG_TEST_VIDEO to an
 * absolute output path; normal test runs do not require ffmpeg. */
#ifndef BOXRIG_TEST_X11_VIDEO_H
#define BOXRIG_TEST_X11_VIDEO_H

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct boxrig_test_video {
    pid_t process;
    int progress;
} boxrig_test_video;

static bool boxrig_test_video_stop(boxrig_test_video *video)
{
    int status = 0;
    if (video->process <= 0) return true;
    kill(video->process, SIGINT);
    while (waitpid(video->process, &status, 0) == -1) {
        if (errno != EINTR) return false;
    }
    close(video->progress);
    video->process = 0;
    return WIFEXITED(status) && (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 255);
}

static bool boxrig_test_video_start(
    boxrig_test_video *video, const char *display, const char *size)
{
    const char *path = getenv("BOXRIG_TEST_VIDEO");
    int descriptors[2];
    char progress[1024] = {0};
    size_t length = 0;
    if (path == NULL) return true;
    if (pipe(descriptors) == -1) return false;
    video->process = fork();
    if (video->process == 0) {
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) == -1) _exit(127);
        close(descriptors[1]);
        execlp("ffmpeg", "ffmpeg", "-nostdin", "-y", "-loglevel", "error",
            "-f", "x11grab", "-framerate", "15", "-video_size", size,
            "-i", display, "-c:v", "libx264", "-preset", "ultrafast",
            "-crf", "18", "-pix_fmt", "yuv420p", "-progress", "pipe:1",
            "-stats_period", "60", path, (char *)NULL);
        _exit(127);
    }
    close(descriptors[1]);
    video->progress = descriptors[0];
    if (video->process < 0) {
        close(descriptors[0]);
        return false;
    }
    while (length + 1 < sizeof(progress)) {
        struct pollfd descriptor = {video->progress, POLLIN, 0};
        ssize_t count;
        if (poll(&descriptor, 1, 15000) <= 0) break;
        count = read(video->progress, progress + length, sizeof(progress) - length - 1);
        if (count <= 0) break;
        length += (size_t)count;
        progress[length] = '\0';
        if (strstr(progress, "frame=") != NULL) return true;
    }
    (void)boxrig_test_video_stop(video);
    return false;
}

static void boxrig_test_video_hold(void)
{
    /* Hold an already asserted state long enough to inspect in the recording.
     * This is not used to decide whether a frame has completed. */
    if (getenv("BOXRIG_TEST_VIDEO") != NULL) (void)poll(NULL, 0, 700);
}

#endif
