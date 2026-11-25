/* boxrig - Under MIT. */
#ifndef BOXRIG_CODEC_H
#define BOXRIG_CODEC_H

#include "unicode.h"

/* UTF-8 arguments are complete events. Newline and modifier transformations
 * affect only text; the final append remains atomic and uses host barriers. */
boxrig_status boxrig_terminal_append_encoded_text(
    boxrig_terminal *terminal, const uint8_t *bytes, size_t length,
    boxrig_newline newline, bool alt, bool eight_bit_meta, bool validated);

#endif
