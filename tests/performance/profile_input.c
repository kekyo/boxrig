/* boxrig - Under MIT. */
#define main text_measurement_main
#include "../measure_text_workloads.c"
#undef main
#include "utf8proc.h"

static bool monitor_enabled;
static size_t attempts, batch_bytes, fallback_graphics, fallback_monitor, fallback_state;
static size_t decoded_bytes, scalars, grapheme_calls, property_calls, width_calls;

static void control_perf(const char *command)
{
    const char *control = getenv("BOXRIG_PERF_CONTROL");
    const char *ack = getenv("BOXRIG_PERF_ACK");
    if (control == NULL || ack == NULL) return;
    FILE *output = fopen(control, "w");
    if (output == NULL || fprintf(output, "%s\n", command) < 0 || fclose(output) != 0) {
        fprintf(stderr, "failed perf control command\n");
        exit(1);
    }
    FILE *input = fopen(ack, "r");
    char response[32];
    if (input == NULL || fgets(response, sizeof(response), input) == NULL ||
        strcmp(response, "ack\n") != 0 || fclose(input) != 0) {
        fprintf(stderr, "failed perf control acknowledgement\n");
        exit(1);
    }
}

void __real_boxrig_text_workload_counters_reset(void);
void __wrap_boxrig_text_workload_counters_reset(void)
{
    __real_boxrig_text_workload_counters_reset();
    control_perf("enable");
}

void __real_boxrig_text_workload_counters_read(boxrig_text_workload_counters *);
void __wrap_boxrig_text_workload_counters_read(boxrig_text_workload_counters *counters)
{
    control_perf("disable");
    __real_boxrig_text_workload_counters_read(counters);
}

boxrig_status __real_boxrig_terminal_create(const boxrig_terminal_config *, boxrig_terminal **);
boxrig_status __wrap_boxrig_terminal_create(
    const boxrig_terminal_config *config, boxrig_terminal **terminal)
{
    boxrig_status status = __real_boxrig_terminal_create(config, terminal);
    if (status == BOXRIG_STATUS_OK && monitor_enabled) {
        static const uint8_t pattern[] = "never-match-this-monitor-pattern";
        boxrig_text_monitor_options options = {
            BOXRIG_TEXT_MONITOR_SCOPE_PRIMARY, BOXRIG_TEXT_MONITOR_FUTURE_ONLY,
            pattern, sizeof(pattern) - 1
        };
        uint64_t id;
        status = boxrig_terminal_register_text_monitor(*terminal, &options, &id);
    }
    return status;
}

#ifndef BOXRIG_INPUT_UNWRAPPED
size_t __real_boxrig_screen_put_ascii_run(boxrig_screen *, const uint8_t *, size_t);
size_t __wrap_boxrig_screen_put_ascii_run(boxrig_screen *screen, const uint8_t *bytes, size_t length)
{
    size_t placed = __real_boxrig_screen_put_ascii_run(screen, bytes, length);
    if (allocation_tracking) {
        attempts++;
        batch_bytes += placed;
        if (placed == 0) {
            if (screen->graphics_sink.text_row_changed != NULL) fallback_graphics++;
            else if (boxrig_terminal_text_monitor_count(screen->owner) != 0) fallback_monitor++;
            else fallback_state++;
        }
    }
    return placed;
}

boxrig_status __real_boxrig_codec_decoder_put(boxrig_codec_decoder *, boxrig_encoding,
    uint8_t, boxrig_unicode_emit, void *);
boxrig_status __wrap_boxrig_codec_decoder_put(boxrig_codec_decoder *decoder,
    boxrig_encoding encoding, uint8_t byte, boxrig_unicode_emit emit, void *context)
{
    if (allocation_tracking) decoded_bytes++;
    return __real_boxrig_codec_decoder_put(decoder, encoding, byte, emit, context);
}

boxrig_status __real_boxrig_screen_put_codepoint(boxrig_screen *, int32_t);
boxrig_status __wrap_boxrig_screen_put_codepoint(boxrig_screen *screen, int32_t codepoint)
{
    if (allocation_tracking) scalars++;
    return __real_boxrig_screen_put_codepoint(screen, codepoint);
}

utf8proc_bool __real_utf8proc_grapheme_break_stateful(utf8proc_int32_t, utf8proc_int32_t, utf8proc_int32_t *);
utf8proc_bool __wrap_utf8proc_grapheme_break_stateful(
    utf8proc_int32_t previous, utf8proc_int32_t current, utf8proc_int32_t *state)
{
    if (allocation_tracking) grapheme_calls++;
    return __real_utf8proc_grapheme_break_stateful(previous, current, state);
}

const utf8proc_property_t *__real_utf8proc_get_property(utf8proc_int32_t);
const utf8proc_property_t *__wrap_utf8proc_get_property(utf8proc_int32_t codepoint)
{
    if (allocation_tracking) property_calls++;
    return __real_utf8proc_get_property(codepoint);
}

int __real_utf8proc_charwidth(utf8proc_int32_t);
int __wrap_utf8proc_charwidth(utf8proc_int32_t codepoint)
{
    if (allocation_tracking) width_calls++;
    return __real_utf8proc_charwidth(codepoint);
}
#endif

int main(int argc, char **argv)
{
    monitor_enabled = getenv("BOXRIG_PROFILE_MONITOR") != NULL;
    int result = text_measurement_main(argc, argv);
    if (result != 0) return result;
    printf("PROFILE input monitor=%d instrumented=%d attempts=%zu batch_bytes=%zu "
        "fallback_graphics=%zu fallback_monitor=%zu fallback_state=%zu "
        "decoded_bytes=%zu scalars=%zu grapheme_calls=%zu property_calls=%zu width_calls=%zu\n",
        monitor_enabled ? 1 : 0,
#ifdef BOXRIG_INPUT_UNWRAPPED
        0,
#else
        1,
#endif
        attempts, batch_bytes, fallback_graphics, fallback_monitor, fallback_state,
        decoded_bytes, scalars, grapheme_calls, property_calls, width_calls);
    return 0;
}
