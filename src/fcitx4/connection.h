/* boxrig - Under MIT. */
#ifndef BOXRIG_FCITX4_CONNECTION_H
#define BOXRIG_FCITX4_CONNECTION_H
#include "../ime/client.h"

/* These hooks run on the client's private context. Destroy follows the
 * common cancellation drain, so no asynchronous callback retains the state. */
void boxrig_fcitx4_initialize(boxrig_ime_client *client);
void boxrig_fcitx4_connect(boxrig_ime_client *client);
void boxrig_fcitx4_stop(boxrig_ime_client *client);
void boxrig_fcitx4_destroy(boxrig_ime_client *client);
#endif
