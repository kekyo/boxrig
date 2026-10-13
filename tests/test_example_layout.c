/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static bool is_regular_file(const char *path)
{
    struct stat status;

    return stat(path, &status) == 0 && S_ISREG(status.st_mode);
}

int main(int argument_count, char **arguments)
{
    static const char *const expected_paths[] = {
        "build/boxrig_term/x11/boxrig_term",
        "build/boxrig_term/gtk4/boxrig_term",
        "build/boxrig_term/gtk3/boxrig_term",
        "build/boxrig_term/wayland/boxrig_term",
        "build/boxrig_term/win32/amd64/boxrig_term.exe",
        "build/boxrig_term/win32/amd64/boxrig_term_launcher.exe",
        "build/boxrig_term/win32/i686/boxrig_term.exe",
        "build/boxrig_term/win32/i686/boxrig_term_launcher.exe",
        "build/boxrig_emitter/posix/boxrig_emitter",
        "build/boxrig_emitter/win32/amd64/boxrig_emitter.exe",
        "build/boxrig_emitter/win32/i686/boxrig_emitter.exe",
    };
    int index;

    if (argument_count != 12) {
        fprintf(
            stderr,
            "usage: %s X11_TERM GTK4_TERM GTK3_TERM WAYLAND_TERM AMD64_TERM "
            "AMD64_LAUNCHER I686_TERM I686_LAUNCHER POSIX_EMITTER "
            "AMD64_EMITTER I686_EMITTER\n",
            arguments[0]);
        return 2;
    }
    for (index = 1; index <= 11; index++) {
        if (strcmp(arguments[index], expected_paths[index - 1]) != 0) {
            fprintf(
                stderr,
                "unexpected example binary path: expected %s, received %s\n",
                expected_paths[index - 1],
                arguments[index]);
            return 1;
        }
        if (!is_regular_file(arguments[index])) {
            fprintf(stderr, "missing example binary: %s\n", arguments[index]);
            return 1;
        }
    }
    printf("PASS terminal example binary layout and names\n");
    return 0;
}
