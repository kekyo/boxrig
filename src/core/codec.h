/* boxrig - Under MIT. */
#ifndef BOXRIG_CODEC_H
#define BOXRIG_CODEC_H

#include "unicode.h"

boxrig_status boxrig_terminal_append_wire(boxrig_terminal *terminal, const uint8_t *bytes, size_t length);
#if BOXRIG_FEATURE_ENCODING_HOOKS
bool boxrig_encoding_barrier_pending(const boxrig_terminal *terminal);
boxrig_status boxrig_encoding_feed(boxrig_terminal *terminal, const uint8_t *bytes, size_t length, size_t *consumed);
boxrig_status boxrig_encoding_finish_input(boxrig_terminal *terminal);
boxrig_status boxrig_encoding_send(boxrig_terminal *terminal, const uint8_t *bytes, size_t length,
    bool finish, bool alt_prefix, bool meta);
#endif

/* UTF-8 arguments are complete events. Newline and modifier transformations
 * affect only text; the final append remains atomic and uses host barriers. */
boxrig_status boxrig_terminal_append_encoded_text(
    boxrig_terminal *terminal, const uint8_t *bytes, size_t length,
    boxrig_newline newline, bool alt, bool eight_bit_meta, bool validated);

#endif
