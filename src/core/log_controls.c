/* boxrig - Under MIT. */
#include "log_controls.h"

#if !BOXRIG_FEATURE_EXTENDED_VT
boxrig_status boxrig_log_dispatch_csi(void *context, uint8_t marker,
    const boxrig_parser_parameter *parameters, size_t count,
    const uint8_t *intermediates, size_t intermediate_count, uint8_t final)
{
    (void)intermediates;
#if BOXRIG_FEATURE_LOG_CURSOR
    if (!intermediate_count && ((marker == '?' && count == 1 && parameters[0].value == 25 &&
        (final == 'h' || final == 'l')) || (!marker && count <= 2 &&
        (final == 'A' || final == 'B' || final == 'C' || final == 'D' ||
         final == 'G' || final == 'H' || final == 'f' || final == 'J')))) {
        if (count < 2 || !parameters[1].subparameter)
            boxrig_log_cursor(context, marker, parameters, count, final);
        return BOXRIG_STATUS_OK;
    }
#endif
    if (marker || intermediate_count) return BOXRIG_STATUS_OK;
    if (final == 'K' && count <= 1) {
        unsigned mode = count ? parameters[0].value : 0;
        if (mode <= 2) boxrig_log_erase(context, mode);
    }
    if (final != 'm') return BOXRIG_STATUS_OK;
    for (size_t i = 0; i < count || (i == 0 && count == 0); ++i) {
        unsigned value = count ? parameters[i].value : 0;
        if (i + 1 < count && parameters[i + 1].subparameter) {
            while (i + 1 < count && parameters[i + 1].subparameter) ++i;
            continue;
        }
        if (value == 38 || value == 48 || value == 58) {
            /* Consume the operands of unsupported indexed and RGB colors. */
            if (i + 1 < count) {
                unsigned format = parameters[++i].value;
                size_t operands = format == 5 ? 1 : format == 2 ? 3 : 0;
                i += operands < count - i ? operands : count - i - 1;
            }
        } else if (value == 0) {
            boxrig_log_set_color(context, true, -1);
            boxrig_log_set_color(context, false, -1);
        } else if (value == 39 || value == 49) {
            boxrig_log_set_color(context, value == 39, -1);
        } else if ((value >= 30 && value <= 37) || (value >= 40 && value <= 47)) {
            boxrig_log_set_color(context, value < 40, (int)(value % 10));
        } else if ((value >= 90 && value <= 97) || (value >= 100 && value <= 107)) {
            boxrig_log_set_color(context, value < 100, (int)(value % 10) + 8);
        }
    }
    return BOXRIG_STATUS_OK;
}
#endif
