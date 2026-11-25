/* boxrig - Under MIT. */
#ifndef BOXRIG_LOG_CONTROLS_H
#define BOXRIG_LOG_CONTROLS_H
#include "parser.h"

/* Screen operations shared by the minimal VT interpreter and compact grid.
 * A negative palette index restores the corresponding default color. */
void boxrig_log_set_color(boxrig_terminal *terminal, bool foreground, int index);
void boxrig_log_erase(boxrig_terminal *terminal, unsigned mode);
#if BOXRIG_FEATURE_LOG_CURSOR
void boxrig_log_cursor(boxrig_terminal *terminal, uint8_t marker, const boxrig_parser_parameter *parameters,
    size_t count, uint8_t final);
#endif
boxrig_status boxrig_log_dispatch_csi(void *context, uint8_t marker,
    const boxrig_parser_parameter *parameters, size_t count,
    const uint8_t *intermediates, size_t intermediate_count, uint8_t final);
#endif
