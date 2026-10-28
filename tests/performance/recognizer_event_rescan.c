#include "boxrig/terminal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "failure at line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(int argument_count, char **arguments)
{
    static const uint8_t pattern[] = "hit";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic;
    uint64_t recognizer_id = 0u;
    size_t consumed = 0u;
    size_t capacity;
    size_t line_count = 3u;
    size_t round;
    uint8_t *input;
    size_t input_length;

    if ((argument_count != 2 && argument_count != 3) ||
        (strcmp(arguments[1], "2") != 0 &&
         strcmp(arguments[1], "8") != 0 &&
         strcmp(arguments[1], "4096") != 0 &&
         strcmp(arguments[1], "8192") != 0) ||
        (argument_count == 3 && strcmp(arguments[2], "4097") != 0)) {
        fprintf(stderr, "usage: %s 2|8 [4097] or %s 4096|8192 4097\n",
            arguments[0], arguments[0]);
        return 2;
    }
    capacity = (size_t)strtoul(arguments[1], NULL, 10);
    if (argument_count == 3) {
        line_count = 4097u;
    }
    input_length = line_count * 5u;
    input = malloc(input_length);
    REQUIRE(input != NULL);
    for (size_t index = 0u; index < line_count; index++) {
        memcpy(input + index * 5u, "hit\r\n", 5u);
    }
    config.rows = 4u;
    config.columns = 16u;
    config.scrollback_capacity = 5000u;
    config.text_recognizer_event_capacity = capacity;
    REQUIRE(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    REQUIRE(boxrig_terminal_feed(
        terminal, input, input_length, &consumed) == BOXRIG_STATUS_OK);
    REQUIRE(consumed == input_length);
    free(input);
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.pattern = pattern;
    options.pattern_length = sizeof(pattern) - 1u;
    options.limits = boxrig_text_recognizer_limits_default();
    REQUIRE(boxrig_terminal_register_text_recognizer(
        terminal, &options, &diagnostic, &recognizer_id) == BOXRIG_STATUS_OK);

    /* Four bounded scans show whether a rescan can return to event loss. */
    for (round = 0u; round < 4u; round++) {
        size_t dispatch;
        size_t work_total = 0u;
        size_t events = 0u;
        size_t kinds[4] = {0u};
        size_t work_budget = line_count > 3u ? 4096u : 128u;
        bool loss = false;

        for (dispatch = 0u; dispatch < 1000u; dispatch++) {
            size_t work_used = 0u;
            bool more_work = false;

            REQUIRE(boxrig_terminal_resume_text_recognizers(
                terminal, work_budget, &work_used, &more_work) == BOXRIG_STATUS_OK);
            REQUIRE(work_used <= work_budget);
            work_total += work_used;
            if (!more_work) {
                break;
            }
        }
        REQUIRE(dispatch < 1000u);
        while (events < line_count + 16u) {
            boxrig_text_recognizer_event event;

            if (!boxrig_terminal_text_recognizer_event_peek(terminal, &event)) {
                break;
            }
            REQUIRE(event.recognizer_id == recognizer_id);
            REQUIRE((size_t)event.kind < 4u);
            kinds[event.kind]++;
            events++;
            if (event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_RESCAN_REQUIRED) {
                loss = true;
                break;
            }
            REQUIRE(boxrig_terminal_text_recognizer_event_consume(terminal) ==
                BOXRIG_STATUS_OK);
        }
        REQUIRE(events < line_count + 16u);
        printf("capacity=%zu lines=%zu round=%zu work=%zu dispatches=%zu events=%zu replaced=%zu evicted=%zu rescan=%zu incomplete=%zu loss=%d needs_rescan=%d\n",
            capacity, line_count, round, work_total, dispatch + 1u, events,
            kinds[BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED],
            kinds[BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_EVICTED],
            kinds[BOXRIG_TEXT_RECOGNIZER_EVENT_RESCAN_REQUIRED],
            kinds[BOXRIG_TEXT_RECOGNIZER_EVENT_SCOPE_INCOMPLETE],
            loss ? 1 : 0,
            boxrig_terminal_text_recognizer_needs_rescan(
                terminal, recognizer_id) ? 1 : 0);
        fflush(stdout);
        if (!loss) {
            break;
        }
        REQUIRE(boxrig_terminal_rescan_text_recognizer(
            terminal, recognizer_id) == BOXRIG_STATUS_OK);
    }
    boxrig_terminal_destroy(terminal);
    return 0;
}
