/* Exercise graphics providers through the sample's real PTY. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <termios.h>
#include <unistd.h>

static bool write_bytes(int descriptor, const void *data, size_t length)
{
    const uint8_t *bytes = data;
    while (length != 0) {
        ssize_t count = write(descriptor, bytes, length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        bytes += count;
        length -= (size_t)count;
    }
    return true;
}

static void encode_name(const char *name, char *output)
{
    const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t length = strlen(name), offset = 0;
    for (size_t index = 0; index < length; index += 3) {
        uint32_t value = (uint32_t)(uint8_t)name[index] << 16;
        if (index + 1 < length) value |= (uint32_t)(uint8_t)name[index + 1] << 8;
        if (index + 2 < length) value |= (uint8_t)name[index + 2];
        output[offset++] = alphabet[value >> 18];
        output[offset++] = alphabet[(value >> 12) & 63];
        output[offset++] = index + 1 < length ? alphabet[(value >> 6) & 63] : '=';
        output[offset++] = index + 2 < length ? alphabet[value & 63] : '=';
    }
    output[offset] = 0;
}

static bool read_response(unsigned int image_id, bool allowed)
{
    char response[256], expected[256];
    size_t length = 0;
    while (length < sizeof(response) - 1) {
        ssize_t count = read(STDIN_FILENO, response + length, 1);
        if (count < 0 && errno == EINTR) continue;
        if (count != 1) return false;
        length++;
        if (length >= 2 && response[length - 2] == '\033' && response[length - 1] == '\\') break;
    }
    response[length] = 0;
    snprintf(expected, sizeof(expected), "\033_Gi=%u;%s\033\\", image_id,
        allowed ? "OK" : "EACCES:external graphics data denied");
    if (strcmp(response, expected) != 0)
        fprintf(stderr, "Unexpected graphics response for image %u: %s\n", image_id, response);
    return strcmp(response, expected) == 0;
}

/* Large direct-transfer frames make reclamation visible in the terminal's RSS,
 * independently of the much smaller animation used for pixel assertions. */
static bool memory_frames(void)
{
    for (unsigned int frame = 0; frame < 8; frame++) {
        size_t remaining = 1024u * 1024u * 4u;
        bool first = true;
        while (remaining != 0) {
            char pixels[3073], encoded[4097], header[160];
            size_t bytes = remaining > 3072 ? 3072 : remaining;
            const unsigned char color[] = {(unsigned char)(71 + frame), 43, 173, 255};
            for (size_t index = 0; index < bytes; index++) pixels[index] = (char)color[index % 4];
            pixels[bytes] = 0;
            encode_name(pixels, encoded);
            remaining -= bytes;
            int length;
            if (!first) length = snprintf(header, sizeof(header), "\033_G%sm=%u;", frame ? "a=f," : "", remaining != 0);
            else if (frame == 0) length = snprintf(header, sizeof(header),
                "\033_Ga=t,i=202,f=32,s=1024,v=1024,m=%u;", remaining != 0);
            else length = snprintf(header, sizeof(header),
                "\033_Ga=f,i=202,f=32,s=1024,v=1024,X=1,z=150,m=%u;", remaining != 0);
            if (length <= 0 || (size_t)length >= sizeof(header) ||
                !write_bytes(STDOUT_FILENO, header, (size_t)length) ||
                !write_bytes(STDOUT_FILENO, encoded, strlen(encoded)) ||
                !write_bytes(STDOUT_FILENO, "\033\\", 2)) return false;
            first = false;
        }
        if (!read_response(202, true)) return false;
    }
    return true;
}

static bool animation_scenario(FILE *report)
{
    const char *setup[] = {
        "\033[3;2H\033_Ga=T,i=101,f=32,s=1,v=1,c=8,r=4,C=1;0yVT/w==\033\\",
        "\033_Ga=f,i=101,f=32,s=1,v=1,X=1,z=150;F79H/w==\033\\",
        "\033_Ga=f,i=101,f=32,s=1,v=1,X=1,z=150;H0/p/w==\033\\"
    };
    for (size_t index = 0; index < sizeof(setup) / sizeof(setup[0]); index++) {
        if (!write_bytes(STDOUT_FILENO, setup[index], strlen(setup[index])) ||
            !read_response(101, true)) return false;
    }
    const char ready[] = "\033[1;1H\033[48;2;71;43;173m  \033[0m"
                         "\033]2;boxrig-graphics-ready\007";
    assert(write_bytes(STDOUT_FILENO, ready, sizeof(ready) - 1));
    fprintf(report, "PASS all\n");
    fflush(report);
    for (;;) {
        char key;
        assert(read(STDIN_FILENO, &key, 1) == 1);
        if (key == 'q') return true;
        if (key == 'm' || key == 'v') {
            if (key == 'm') {
                if (!memory_frames()) return false;
            } else {
                const char remove[] = "\033_Ga=d,d=I,i=202\033\\";
                if (!write_bytes(STDOUT_FILENO, remove, sizeof(remove) - 1) ||
                    !read_response(202, true)) return false;
            }
            fprintf(report, "PASS %c\n", key);
            fflush(report);
            continue;
        }
        const char *control;
        unsigned int phase;
        switch (key) {
            case 's':
                control = "\033_Ga=a,i=101,r=1,z=150,c=1,s=3,v=1\033\\";
                phase = 1;
                break;
            case 'd':
                control = "\033_Ga=d,d=f,i=101,r=2\033\\";
                phase = 2;
                break;
            case 'f':
                control = "\033_Ga=d,d=f,i=101,r=1\033\\";
                phase = 3;
                break;
            case 'x':
                control = "\033_Ga=d,d=F,i=101\033\\";
                phase = 4;
                break;
            case 'a':
                control = "\033_Ga=f,i=101,f=32,s=1,v=1,X=1,z=150;77UN/w==\033\\";
                phase = 5;
                break;
            case 'l':
                control = "\033_Ga=d,d=f,i=101,r=4\033\\";
                phase = 6;
                break;
            case 'c':
                /* Keep the selected frame playing long enough to observe it
                 * before the controller deletes that current frame. */
                control = "\033_Ga=a,i=101,r=2,z=2000,c=2,s=3,v=1\033\\";
                phase = 7;
                break;
            default: return false;
        }
        char operation[160];
        int length = snprintf(operation, sizeof(operation),
            "%s\033[1;1H\033[48;2;%u;43;173m  \033[0m", control, (71 + phase * 31) % 256);
        assert(length > 0 && (size_t)length < sizeof(operation));
        if (!write_bytes(STDOUT_FILENO, operation, (size_t)length) ||
            !read_response(101, true)) return false;
        fprintf(report, "PASS %c\n", key);
        fflush(report);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    bool allowed = strcmp(argv[2], "allow") == 0;
    FILE *report = fopen(argv[1], "w");
    assert(report != NULL);
    struct termios original, raw;
    assert(tcgetattr(STDIN_FILENO, &original) == 0);
    raw = original;
    cfmakeraw(&raw);
    assert(tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0);
    const char initial[] = "\033[?25l\033[2J\033[HExternal image transport\033]2;boxrig-graphics-loading\007";
    assert(write_bytes(STDOUT_FILENO, initial, sizeof(initial) - 1));
    if (strcmp(argv[2], "animation") == 0) {
        bool success = animation_scenario(report);
        if (!success) fprintf(report, "FAIL animation\n");
        assert(tcsetattr(STDIN_FILENO, TCSANOW, &original) == 0);
        assert(fclose(report) == 0);
        return success ? 0 : 1;
    }
    const uint8_t colors[][4] = {{211, 37, 83, 255}, {23, 191, 71, 255}, {31, 79, 233, 255}};
    const char media[] = {'f', 't', 's'};
    bool success = true;
    for (size_t index = 0; index < 3 && success; index++) {
        char name[160], encoded[256], control[512];
        int descriptor;
        if (media[index] == 's') {
            snprintf(name, sizeof(name), "/boxrig-graphics-%ld", (long)getpid());
            descriptor = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
        } else {
            snprintf(name, sizeof(name), "/tmp/boxrig-tty-graphics-protocol-%ld-%c-XXXXXX",
                (long)getpid(), media[index]);
            descriptor = mkstemp(name);
        }
        assert(descriptor != -1);
        uint8_t pixels[2 + 16 * 16 * 4] = {1, 2};
        for (size_t at = 2; at < sizeof(pixels); at += 4) memcpy(pixels + at, colors[index], 4);
        assert(write_bytes(descriptor, pixels, sizeof(pixels)));
        assert(close(descriptor) == 0);
        encode_name(name, encoded);
        unsigned int image_id = 101 + (unsigned int)index;
        int length = snprintf(control, sizeof(control),
            "\033[3;%zuH\033_Ga=T,t=%c,f=32,s=16,v=16,i=%u,O=2,S=1024,c=4,r=2,C=1;%s\033\\",
            2 + index * 8, media[index], image_id, encoded);
        assert(length > 0 && (size_t)length < sizeof(control));
        success = write_bytes(STDOUT_FILENO, control, (size_t)length) && read_response(image_id, allowed);
        if (media[index] == 's') {
            descriptor = shm_open(name, O_RDONLY, 0);
            success = success && (allowed ? descriptor == -1 && errno == ENOENT : descriptor != -1);
            if (descriptor != -1) (void)close(descriptor);
            (void)shm_unlink(name);
        } else {
            bool exists = access(name, F_OK) == 0;
            success = success && (allowed && media[index] == 't' ? !exists : exists);
            (void)unlink(name);
        }
        fprintf(report, "%s %c\n", success ? "PASS" : "FAIL", media[index]);
        fflush(report);
    }
    if (success) {
        const char ready[] = "\033]2;boxrig-graphics-ready\007";
        assert(write_bytes(STDOUT_FILENO, ready, sizeof(ready) - 1));
        fprintf(report, "PASS all\n");
        fflush(report);
        char command;
        assert(read(STDIN_FILENO, &command, 1) == 1 && command == 'q');
    }
    assert(tcsetattr(STDIN_FILENO, TCSANOW, &original) == 0);
    assert(fclose(report) == 0);
    return success ? 0 : 1;
}
