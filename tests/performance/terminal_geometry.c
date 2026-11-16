/* boxrig - Under MIT. */
#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

int main(void)
{
    struct winsize size;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) != 0 ||
        size.ws_row == 0 || size.ws_col == 0) return 1;
    /* POSIX PTYs expose rows/columns but boxrig deliberately leaves their
     * optional pixel fields empty. Query the terminal's actual cell metrics. */
    int tty = open("/dev/tty", O_RDWR | O_NOCTTY);
    struct termios original, raw;
    if (tty < 0 || tcgetattr(tty, &original) != 0) return 1;
    raw = original;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 50;
    if (tcsetattr(tty, TCSANOW, &raw) != 0) return 1;
    char reply[64] = {0};
    size_t length = 0;
    if (write(tty, "\033[16t", 5) == 5) {
        while (length < sizeof(reply) - 1) {
            if (read(tty, reply + length, 1) != 1) break;
            if (reply[length++] == 't') break;
        }
    }
    int restored = tcsetattr(tty, TCSANOW, &original);
    close(tty);
    unsigned int width = 0, height = 0;
    if (restored != 0 || sscanf(reply, "\033[6;%u;%ut", &height, &width) != 2 ||
        width == 0 || height == 0 || width > 4096 || height > 4096) return 1;
    printf("{\"rows\":%u,\"columns\":%u,\"pixel_width\":%u,\"pixel_height\":%u,"
           "\"cell_width\":%u,\"cell_height\":%u}\n",
        size.ws_row, size.ws_col, width * size.ws_col, height * size.ws_row,
        width, height);
    return 0;
}
