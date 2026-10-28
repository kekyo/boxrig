/* boxrig - Under MIT. */
#define _POSIX_C_SOURCE 200809L
#include "text_policy.h"
#include "boxrig/ui.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static size_t registrations;

boxrig_status __real_boxrig_terminal_register_text_recognizer(
    boxrig_terminal *terminal, const boxrig_text_recognizer_options *options,
    boxrig_text_regex_diagnostic *diagnostic, uint64_t *id);

boxrig_status __wrap_boxrig_terminal_register_text_recognizer(
    boxrig_terminal *terminal, const boxrig_text_recognizer_options *options,
    boxrig_text_regex_diagnostic *diagnostic, uint64_t *id)
{
    registrations++;
    return __real_boxrig_terminal_register_text_recognizer(
        terminal, options, diagnostic, id);
}

static void require(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", message);
        exit(1);
    }
}

static uint64_t now_ns(void)
{
    struct timespec value;
    require(clock_gettime(CLOCK_MONOTONIC, &value) == 0, "clock");
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static size_t number(const char *text, size_t maximum)
{
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    require(text[0] >= '0' && text[0] <= '9' && end != text &&
        *end == '\0' && value <= maximum, "number");
    return (size_t)value;
}

static void feed(boxrig_terminal *terminal, size_t index, bool positive)
{
    char text[80];
    int length = snprintf(text, sizeof(text), positive ?
        "https://example.org/%zu\r\n" : "plain %zu\r\n", index);
    size_t consumed = 0;
    require(length > 0 && (size_t)length < sizeof(text), "format");
    require(boxrig_terminal_feed(terminal, (const uint8_t *)text, (size_t)length,
        &consumed) == BOXRIG_STATUS_OK && consumed == (size_t)length, "feed");
}

static void drain(boxrig_term_text_policy *policy, size_t budget,
    size_t *work, size_t *dispatches, uint64_t *maximum)
{
    bool more = true;
    for (size_t index = 0; more; index++) {
        size_t used = 0;
        uint64_t start = now_ns();
        require(boxrig_term_text_policy_resume(policy, budget, &used, &more) == 0,
            "resume policy");
        uint64_t elapsed = now_ns() - start;
        if (elapsed > *maximum) *maximum = elapsed;
        require(index < 1000000 && used <= budget, "bounded completion");
        *work += used;
        (*dispatches)++;
    }
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s history period updates budget\n", argv[0]);
        return 2;
    }
    size_t history = number(argv[1], 100000);
    size_t period = number(argv[2], 100000);
    size_t updates = number(argv[3], 100000);
    size_t budget = number(argv[4], 1000000);
    require(period != 0 && updates != 0 && budget != 0, "positive arguments");
    require(unsetenv("BOXRIG_AUTO_URLS") == 0, "enable sample URLs");
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_term_text_policy *policy = NULL;
    config.rows = 24;
    config.columns = 80;
    config.scrollback_capacity = history;
    require(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK, "terminal");
    for (size_t index = 0; index < history + 23; index++)
        feed(terminal, index, index % period == 0);
    require(boxrig_terminal_scrollback_rows(terminal) == history, "full history");
    require(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK &&
        boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK &&
        boxrig_term_text_policy_create(terminal, view, &policy) == 0, "policy");
    size_t work = 0, dispatches = 0;
    uint64_t maximum = 0, start = now_ns();
    drain(policy, budget, &work, &dispatches, &maximum);
    uint64_t initial_ns = now_ns() - start;
    printf("MEASURE policy phase=initial history=%zu period=%zu updates=0 "
        "budget=%zu wall_ns=%" PRIu64 " maximum_dispatch_ns=%" PRIu64
        " work=%zu dispatches=%zu registrations=%zu\n", history, period,
        budget, initial_ns, maximum, work, dispatches, registrations);
    registrations = 0;
    work = 0;
    dispatches = 0;
    maximum = 0;
    start = now_ns();
    for (size_t index = 0; index < updates; index++) {
        feed(terminal, history + 23 + index, true);
        drain(policy, budget, &work, &dispatches, &maximum);
        require(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK, "synchronize");
    }
    uint64_t steady_ns = now_ns() - start;
    size_t stopped_work = 0, stopped_dispatches = 0;
    uint64_t stopped_maximum = 0;
    drain(policy, budget, &stopped_work, &stopped_dispatches, &stopped_maximum);
    require(stopped_work == 0 && stopped_dispatches == 1, "idle after output stops");
    printf("MEASURE policy phase=steady history=%zu period=%zu updates=%zu "
        "budget=%zu wall_ns=%" PRIu64 " maximum_dispatch_ns=%" PRIu64
        " work=%zu dispatches=%zu registrations=%zu stopped_work=%zu\n",
        history, period, updates, budget, steady_ns, maximum, work,
        dispatches, registrations, stopped_work);
    boxrig_term_text_policy_destroy(policy);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return 0;
}
