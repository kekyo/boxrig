/* boxrig - Under MIT. */
#ifndef BOXRIG_TEST_GTK_IME_SERVER_H
#define BOXRIG_TEST_GTK_IME_SERVER_H
#include <stdint.h>
#include <sys/types.h>

/* A private SOCK_SEQPACKET control channel keeps the compositor alive while
 * GTK performs synchronous display initialization. No desktop is contacted. */
enum ime_server_operation {
    IME_SERVER_QUERY, IME_SERVER_PREEDIT, IME_SERVER_COMMIT,
    IME_SERVER_CANCEL, IME_SERVER_FOCUS, IME_SERVER_KEY, IME_SERVER_PIXELS, IME_SERVER_STOP, IME_SERVER_BLUR_CAPABILITY
};
typedef struct ime_server_command {
    int operation, begin, end, x, y, cell_width, cell_height;
    char text[128];
} ime_server_command;
typedef struct ime_server_state {
    uint32_t version, contexts, enabled, enable_count, disable_count;
    uint32_t commits, frames, selected_cells;
    uint32_t blur_objects, blur_destroyed, blur_commits, blur_enabled;
    int blur_x, blur_y, blur_width, blur_height;
    int x, y, width, height;
    char socket[32];
} ime_server_state;
int ime_server_start(unsigned int version, unsigned int scale, pid_t *pid);
ime_server_state ime_server_exchange(int channel, ime_server_command command);
void ime_server_stop(int channel, pid_t pid);
#endif
